#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/bootmem.h>
#include <linux/page_ext.h>
#include <linux/memory.h>
#include <linux/vmalloc.h>
#include <linux/kmemleak.h>
#include <linux/page_owner.h>

/*
 * struct page extension
 *
 * This is the feature to manage memory for extended data per page.
 *
 * Until now, we must modify struct page itself to store extra data per page.
 * This requires rebuilding the kernel and it is really time consuming process.
 * And, sometimes, rebuild is impossible due to third party module dependency.
 * At last, enlarging struct page could cause un-wanted system behaviour change.
 *
 * This feature is intended to overcome above mentioned problems. This feature
 * allocates memory for extended data per page in certain place rather than
 * the struct page itself. This memory can be accessed by the accessor
 * functions provided by this code. During the boot process, it checks whether
 * allocation of huge chunk of memory is needed or not. If not, it avoids
 * allocating memory at all. With this advantage, we can include this feature
 * into the kernel in default and can avoid rebuild and solve related problems.
 *
 * To help these things to work well, there are two callbacks for clients. One
 * is the need callback which is mandatory if user wants to avoid useless
 * memory allocation at boot-time. The other is optional, init callback, which
 * is used to do proper initialization after memory is allocated.
 *
 * The need callback is used to decide whether extended memory allocation is
 * needed or not. Sometimes users want to deactivate some features in this
 * boot and extra memory would be unneccessary. In this case, to avoid
 * allocating huge chunk of memory, each clients represent their need of
 * extra memory through the need callback. If one of the need callbacks
 * returns true, it means that someone needs extra memory so that
 * page extension core should allocates memory for page extension. If
 * none of need callbacks return true, memory isn't needed at all in this boot
 * and page extension core can skip to allocate memory. As result,
 * none of memory is wasted.
 *
 * The init callback is used to do proper initialization after page extension
 * is completely initialized. In sparse memory system, extra memory is
 * allocated some time later than memmap is allocated. In other words, lifetime
 * of memory for page extension isn't same with memmap for struct page.
 * Therefore, clients can't store extra data until page extension is
 * initialized, even if pages are allocated and used freely. This could
 * cause inadequate state of extra data per page, so, to prevent it, client
 * can utilize this callback to initialize the state of it correctly.
 */

/* IAMROOT-12AB:
 * -------------
 * page_ext를 사용하는 3가지 디버깅 방법
 *
 * 각각의 디버깅용 콜백 구조체 ops에 2개의 함수가 연결되어 있다.
 *	- need_로 시작되는 함수는 이 기능을 사용하는지 여부를 체크 
 *	- init_으로 시자되는 함수는 이 기능을 사용하는 경우 초기화를 위한 
 *	  함수를 호출하게 된다.
 *
 * 3개의 구조체 모두 컴파일 타임에 빌드에 포함되지 않을 경우 구조체를 만들 수가
 * 없으므로 첫 항목에는 이 기능을 사용하지 않을 경우에도 항목이 존재하게 두었고 
 * 그 내부 콜백 변수를 null로 초기화 한다.
 *
 * 각각의 기능들을 사용하기 위해 컴파일 옵션과 커널 파라메터를 켜서 사용한다.
 *	1) Guard Page: 
 *		- CONFIG_DEBUG_PAGEALLOC
 *		- "debug_pagealloc=on"
 *	2) Posioning:
 *		- CONFIG_PAGE_POISONING 
 *		- "debug_pagealloc=on"
 *	3) Page Owner:
 *		- CONFIG_PAGE_OWNER 
 *		- "page_owner=on"
 */
static struct page_ext_operations *page_ext_ops[] = {
	&debug_guardpage_ops,
#ifdef CONFIG_PAGE_POISONING
	&page_poisoning_ops,
#endif
#ifdef CONFIG_PAGE_OWNER
	&page_owner_ops,
#endif
};

/* IAMROOT-12AB:
 * -------------
 * page_ext 구조체에 사용되는 전체 노드 메모리의 양을 출력하기 위해 사용(pages)
 */
static unsigned long total_usage;

/* IAMROOT-12AB:
 * -------------
 * 등록되어 있는 page_ext_ops[]의 need에 연결된 함수들 중 하나의 결과가 true가 
 * 발생하면 true를 반환한다. 그렇지 않으면 false
 */
static bool __init invoke_need_callbacks(void)
{
	int i;
	int entries = ARRAY_SIZE(page_ext_ops);

/* IAMROOT-12AB:
 * -------------
 * 컴파일 옵션에 따라 1~3개의 엔트리를 가진다.
 */
	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->need && page_ext_ops[i]->need())
			return true;
	}

	return false;
}

static void __init invoke_init_callbacks(void)
{
	int i;
	int entries = ARRAY_SIZE(page_ext_ops);

/* IAMROOT-12AB:
 * -------------
 * page_ext_ops[]->init에 등록된 초기화 함수를 호출한다.
 */
	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->init)
			page_ext_ops[i]->init();
	}
}

#if !defined(CONFIG_SPARSEMEM)


void __meminit pgdat_page_ext_init(struct pglist_data *pgdat)
{
	pgdat->node_page_ext = NULL;
}


/* IAMROOT-12AB:
 * -------------
 * sparse 메모리 모델이 아닌 경우 page로 node_page_ext를 
 * 찾은 후 page_ext를 찾아 반환한다.
 */
struct page_ext *lookup_page_ext(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long offset;
	struct page_ext *base;

	base = NODE_DATA(page_to_nid(page))->node_page_ext;
#ifdef CONFIG_DEBUG_VM
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_ext arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	if (unlikely(!base))
		return NULL;
#endif

/* IAMROOT-12AB:
 * -------------
 * offset은 mem_map에서 구한 후 함수에서 반환 시에 base(page_ext *)에 
 * offset을 더해 반환하여 page_ext 포인터 주소를 알려준다.
 */
	offset = pfn - round_down(node_start_pfn(page_to_nid(page)),
					MAX_ORDER_NR_PAGES);
	return base + offset;
}

static int __init alloc_node_page_ext(int nid)
{
	struct page_ext *base;
	unsigned long table_size;
	unsigned long nr_pages;

/* IAMROOT-12AB:
 * -------------
 * page_ext 구조체용 메모리를 페이지 수 만큼 할당한다.
 * (정렬되어 있지 않은 경우 MAX_ORDER_NR_PAGES(2^(MAX_ORDER-1))만큼 추가 할당한다)
 * 노드 구조체의 node_page_ext는 이 page_ext를 가리킨다.
 */
	nr_pages = NODE_DATA(nid)->node_spanned_pages;
	if (!nr_pages)
		return 0;

	/*
	 * Need extra space if node range is not aligned with
	 * MAX_ORDER_NR_PAGES. When page allocator's buddy algorithm
	 * checks buddy's status, range could be out of exact node range.
	 */
	if (!IS_ALIGNED(node_start_pfn(nid), MAX_ORDER_NR_PAGES) ||
		!IS_ALIGNED(node_end_pfn(nid), MAX_ORDER_NR_PAGES))
		nr_pages += MAX_ORDER_NR_PAGES;

	table_size = sizeof(struct page_ext) * nr_pages;

	base = memblock_virt_alloc_try_nid_nopanic(
			table_size, PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
			BOOTMEM_ALLOC_ACCESSIBLE, nid);
	if (!base)
		return -ENOMEM;
	NODE_DATA(nid)->node_page_ext = base;
	total_usage += table_size;
	return 0;
}


/* IAMROOT-12AB:
 * -------------
 * FLATMEM을 사용하는 32bit ARM에서 사용
 */
void __init page_ext_init_flatmem(void)
{
	int nid, fail;

/* IAMROOT-12AB:
 * -------------
 * 디버깅을 위해 page_ext 구조체 할당이 필요한지 여부를 체크한다.
 *
 * page_ext_ops[]->need에 등록된 함수들에서 하나도 true 발생하지 않으면 return 
 */
	if (!invoke_need_callbacks())
		return;

/* IAMROOT-12AB:
 * -------------
 * 노드가 사용하는 페이지 만큼 page_ext[]를 각 노드에 할당한다.
 * 각 노드의 node_page_ext는 할당된 메모리를 가리킨다.
 */
	for_each_online_node(nid)  {
		fail = alloc_node_page_ext(nid);
		if (fail)
			goto fail;
	}
	pr_info("allocated %ld bytes of page_ext\n", total_usage);
	invoke_init_callbacks();
	return;

fail:
	pr_crit("allocation of page_ext failed.\n");
	panic("Out of memory");
}

#else /* CONFIG_FLAT_NODE_MEM_MAP */

/* IAMROOT-12AB:
 * -------------
 * SPARSE 메모리 모델인 경우 page로 mem_section을 구한 후 
 * page_ext를 찾는다.
 */
struct page_ext *lookup_page_ext(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct mem_section *section = __pfn_to_section(pfn);
#ifdef CONFIG_DEBUG_VM
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_ext arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	if (!section->page_ext)
		return NULL;
#endif
	return section->page_ext + pfn;
}

static void *__meminit alloc_page_ext(size_t size, int nid)
{
	gfp_t flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN;
	void *addr = NULL;

	addr = alloc_pages_exact_nid(nid, size, flags);
	if (addr) {
		kmemleak_alloc(addr, size, 1, flags);
		return addr;
	}

	if (node_state(nid, N_HIGH_MEMORY))
		addr = vzalloc_node(size, nid);
	else
		addr = vzalloc(size);

	return addr;
}

static int __meminit init_section_page_ext(unsigned long pfn, int nid)
{
	struct mem_section *section;
	struct page_ext *base;
	unsigned long table_size;

	section = __pfn_to_section(pfn);

	if (section->page_ext)
		return 0;

	table_size = sizeof(struct page_ext) * PAGES_PER_SECTION;
	base = alloc_page_ext(table_size, nid);

	/*
	 * The value stored in section->page_ext is (base - pfn)
	 * and it does not point to the memory block allocated above,
	 * causing kmemleak false positives.
	 */
	kmemleak_not_leak(base);

	if (!base) {
		pr_err("page ext allocation failure\n");
		return -ENOMEM;
	}

	/*
	 * The passed "pfn" may not be aligned to SECTION.  For the calculation
	 * we need to apply a mask.
	 */
	pfn &= PAGE_SECTION_MASK;
	section->page_ext = base - pfn;
	total_usage += table_size;
	return 0;
}
#ifdef CONFIG_MEMORY_HOTPLUG
static void free_page_ext(void *addr)
{
	if (is_vmalloc_addr(addr)) {
		vfree(addr);
	} else {
		struct page *page = virt_to_page(addr);
		size_t table_size;

		table_size = sizeof(struct page_ext) * PAGES_PER_SECTION;

		BUG_ON(PageReserved(page));
		free_pages_exact(addr, table_size);
	}
}

static void __free_page_ext(unsigned long pfn)
{
	struct mem_section *ms;
	struct page_ext *base;

	ms = __pfn_to_section(pfn);
	if (!ms || !ms->page_ext)
		return;
	base = ms->page_ext + pfn;
	free_page_ext(base);
	ms->page_ext = NULL;
}

static int __meminit online_page_ext(unsigned long start_pfn,
				unsigned long nr_pages,
				int nid)
{
	unsigned long start, end, pfn;
	int fail = 0;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	if (nid == -1) {
		/*
		 * In this case, "nid" already exists and contains valid memory.
		 * "start_pfn" passed to us is a pfn which is an arg for
		 * online__pages(), and start_pfn should exist.
		 */
		nid = pfn_to_nid(start_pfn);
		VM_BUG_ON(!node_state(nid, N_ONLINE));
	}

	for (pfn = start; !fail && pfn < end; pfn += PAGES_PER_SECTION) {
		if (!pfn_present(pfn))
			continue;
		fail = init_section_page_ext(pfn, nid);
	}
	if (!fail)
		return 0;

	/* rollback */
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__free_page_ext(pfn);

	return -ENOMEM;
}

static int __meminit offline_page_ext(unsigned long start_pfn,
				unsigned long nr_pages, int nid)
{
	unsigned long start, end, pfn;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__free_page_ext(pfn);
	return 0;

}

static int __meminit page_ext_callback(struct notifier_block *self,
			       unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	int ret = 0;

	switch (action) {
	case MEM_GOING_ONLINE:
		ret = online_page_ext(mn->start_pfn,
				   mn->nr_pages, mn->status_change_nid);
		break;
	case MEM_OFFLINE:
		offline_page_ext(mn->start_pfn,
				mn->nr_pages, mn->status_change_nid);
		break;
	case MEM_CANCEL_ONLINE:
		offline_page_ext(mn->start_pfn,
				mn->nr_pages, mn->status_change_nid);
		break;
	case MEM_GOING_OFFLINE:
		break;
	case MEM_ONLINE:
	case MEM_CANCEL_OFFLINE:
		break;
	}

	return notifier_from_errno(ret);
}

#endif

void __init page_ext_init(void)
{
	unsigned long pfn;
	int nid;

	if (!invoke_need_callbacks())
		return;

	for_each_node_state(nid, N_MEMORY) {
		unsigned long start_pfn, end_pfn;

		start_pfn = node_start_pfn(nid);
		end_pfn = node_end_pfn(nid);
		/*
		 * start_pfn and end_pfn may not be aligned to SECTION and the
		 * page->flags of out of node pages are not initialized.  So we
		 * scan [start_pfn, the biggest section's pfn < end_pfn) here.
		 */
		for (pfn = start_pfn; pfn < end_pfn;
			pfn = ALIGN(pfn + 1, PAGES_PER_SECTION)) {

			if (!pfn_valid(pfn))
				continue;
			/*
			 * Nodes's pfns can be overlapping.
			 * We know some arch can have a nodes layout such as
			 * -------------pfn-------------->
			 * N0 | N1 | N2 | N0 | N1 | N2|....
			 */
			if (pfn_to_nid(pfn) != nid)
				continue;
			if (init_section_page_ext(pfn, nid))
				goto oom;
		}
	}
	hotplug_memory_notifier(page_ext_callback, 0);
	pr_info("allocated %ld bytes of page_ext\n", total_usage);
	invoke_init_callbacks();
	return;

oom:
	panic("Out of memory");
}

void __meminit pgdat_page_ext_init(struct pglist_data *pgdat)
{
}

#endif
