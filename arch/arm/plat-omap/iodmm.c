/*
 * OMAP DMM (Dynamic memory mapping) to IOMMU module
 *
 * Copyright (C) 2010 Texas Instruments. All rights reserved.
 *
 * Authors: Ramesh Gupta <grgupta@ti.com>
 *          Hari Kanigeri <h-kanigeri2@ti.com>
 *          Ohad Ben-Cohen <ohad@wizery.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#if 0
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/pagemap.h>
#include <linux/kernel.h>
#include <linux/genalloc.h>

#include <asm/cacheflush.h>
#include <asm/mach/map.h>

#include <plat/iommu.h>
#include <plat/iovmm.h>

#include "iopgtable.h"

/* Hack hack code to handle MM buffers */
int temp_user_dma_op(unsigned long start, unsigned long end, int op)
{

	struct mm_struct *mm = current->active_mm;
	void (*inner_op)(const void *, const void *);
	void (*outer_op)(unsigned long, unsigned long);

	switch (op) {
	case 1:
		inner_op = dmac_inv_range;
		outer_op = outer_inv_range;
		break;

	case 2:
		inner_op = dmac_clean_range;
		outer_op = outer_clean_range;
		break;

	case 3:
		inner_op = dmac_flush_range;
		outer_op = outer_flush_range;
		break;

	default:
		return -EINVAL;
	}

	if (end < start)
		return -EINVAL;

	down_read(&mm->mmap_sem);

	do {
		struct vm_area_struct *vma = find_vma(mm, start);

		if (!vma || start < vma->vm_start ||
			vma->vm_flags & (VM_IO | VM_PFNMAP)) {
			up_read(&mm->mmap_sem);
			return -EFAULT;
		}
		do {
			unsigned long e = (start | ~PAGE_MASK) + 1;
			struct page *page;

			if (e > end)
				e = end;
			page = follow_page(vma, start, FOLL_GET);
			if (IS_ERR(page)) {
				up_read(&mm->mmap_sem);
				return PTR_ERR(page);
			}
			if (page) {
				unsigned long phys;
				/*
				 * This flushes the userspace address - which
				 * is not what this API was intended to do.
				 * Things may go astray as a result.
				 */
				inner_op((void *)start, (void *)e);
				/*
				 * Now handle the L2 cache.
				 */
				phys = page_to_phys(page) +
						(start & ~PAGE_MASK);
				outer_op(phys, phys + e - start);
				put_page(page);
			}
			start = e;
		} while (start < end && start < vma->vm_end);
	} while (start < end);

	up_read(&mm->mmap_sem);
	return 0;
}

/* remember mapping information */
struct dmm_map_object *add_mapping_info(struct iodmm_struct *obj,
		struct gen_pool *gen_pool, u32 va, u32 da, u32 size)
{
	struct dmm_map_object *map_obj;

	u32 num_usr_pgs = size / PAGE_SIZE;

	pr_debug("%s: adding map info: va 0x%x virt 0x%x size 0x%x\n",
						__func__, va,
						da, size);
	map_obj = kzalloc(sizeof(struct dmm_map_object), GFP_KERNEL);
	if (!map_obj) {
		pr_err("%s: kzalloc failed\n", __func__);
		return NULL;
	}
	INIT_LIST_HEAD(&map_obj->link);

	map_obj->pages = kcalloc(num_usr_pgs, sizeof(struct page *),
							GFP_KERNEL);
	if (!map_obj->pages) {
		pr_err("%s: kzalloc failed\n", __func__);
		kfree(map_obj);
		return NULL;
	}

	map_obj->va = va;
	map_obj->da = da;
	map_obj->size = size;
	map_obj->num_usr_pgs = num_usr_pgs;
	map_obj->gen_pool = gen_pool;
	spin_lock(&obj->dmm_map_lock);
	list_add(&map_obj->link, &obj->map_list);
	spin_unlock(&obj->dmm_map_lock);

	return map_obj;
}

static int match_exact_map_obj(struct dmm_map_object *map_obj,
					u32 da, u32 size)
{
	u32 res;

	if (map_obj->da == da && map_obj->size != size)
		pr_err("%s: addr match (0x%x), size don't (0x%x != 0x%x)\n",
				__func__, da, map_obj->size, size);

	if (map_obj->da == da && map_obj->size == size)
		res = 0;
	else
		res = -ENODATA;
	return res;
}

static void remove_mapping_information(struct iodmm_struct *obj,
						u32 da, u32 size)
{
	struct dmm_map_object *map_obj;

	pr_debug("%s: looking for virt 0x%x size 0x%x\n", __func__,
							da, size);
	spin_lock(&obj->dmm_map_lock);
	list_for_each_entry(map_obj, &obj->map_list, link) {
		pr_debug("%s: candidate: va 0x%x virt 0x%x size 0x%x\n",
							__func__,
							map_obj->va,
							map_obj->da,
							map_obj->size);

		if (!match_exact_map_obj(map_obj, da, size)) {
			pr_debug("%s: match, deleting map info\n", __func__);
			if (map_obj->gen_pool != NULL)
				gen_pool_free(map_obj->gen_pool, da, size);
			list_del(&map_obj->link);
			kfree(map_obj->dma_info.sg);
			kfree(map_obj->pages);
			kfree(map_obj);
			goto out;
		}
		pr_debug("%s: candidate didn't match\n", __func__);
	}

	pr_err("%s: failed to find given map info\n", __func__);
out:
	spin_unlock(&obj->dmm_map_lock);
}

static int match_containing_map_obj(struct dmm_map_object *map_obj,
				u32 va, u32 da, bool check_va, u32 size)
{
	u32 res;
	u32 map_obj_end;

	if (check_va) {
		map_obj_end = map_obj->va + map_obj->size;
		if ((va >= map_obj->va) && (va + size <= map_obj_end))
			res = 0;
		else
			res = -ENODATA;
	} else {
		if (da == map_obj->da)
			res = 0;
		else
			res = -ENODATA;
	}
	return res;
}

static struct dmm_map_object *find_containing_mapping(
				struct iodmm_struct *obj,
				u32 va, u32 da, bool check_va,
				u32 size)
{
	struct dmm_map_object *map_obj, *temp_map;
	pr_debug("%s: looking for va 0x%x size 0x%x\n", __func__,
						va, size);

	spin_lock(&obj->dmm_map_lock);
	list_for_each_entry_safe(map_obj, temp_map, &obj->map_list, link) {
		pr_debug("%s: candidate: va 0x%x virt 0x%x size 0x%x\n",
						__func__,
						map_obj->va,
						map_obj->da,
						map_obj->size);
		if (!match_containing_map_obj(map_obj, va, da, check_va,
							map_obj->size)) {
			pr_debug("%s: match!\n", __func__);
			goto out;
		}

		pr_debug("%s: no match!\n", __func__);
	}

	map_obj = NULL;
out:
	spin_unlock(&obj->dmm_map_lock);
	return map_obj;
}

#if 0
static int find_first_page_in_cache(struct dmm_map_object *map_obj,
					unsigned long va)
{
	u32 mapped_base_page = map_obj->va >> PAGE_SHIFT;
	u32 requested_base_page = va >> PAGE_SHIFT;
	int pg_index = requested_base_page - mapped_base_page;

	if (pg_index < 0 || pg_index >= map_obj->num_usr_pgs) {
		pr_err("%s: failed (got %d)\n", __func__, pg_index);
		return -1;
	}

	pr_debug("%s: first page is %d\n", __func__, pg_index);
	return pg_index;
}
#endif

static inline struct page *get_mapping_page(struct dmm_map_object *map_obj,
								int pg_i)
{
	pr_debug("%s: looking for pg_i %d, num_usr_pgs: %d\n", __func__,
					pg_i, map_obj->num_usr_pgs);
	if (pg_i < 0 || pg_i >= map_obj->num_usr_pgs) {
		pr_err("%s: requested pg_i %d is out of mapped range\n",
				__func__, pg_i);
		return NULL;
	}
	return map_obj->pages[pg_i];
}

#if 0
/* Cache operation against kernel address instead of users */
static int build_dma_sg(struct dmm_map_object *map_obj, unsigned long start,
						ssize_t len, int pg_i)
{
	struct page *page;
	unsigned long offset;
	ssize_t rest;
	int ret = 0, i = 0;
	struct scatterlist *sg = map_obj->dma_info.sg;

	while (len) {
		page = get_mapping_page(map_obj, pg_i);
		if (!page) {
			pr_err("%s: no page for %08lx, pg_i is %x\n", __func__,
								start, pg_i);
			ret = -EINVAL;
			goto out;
		} else if (IS_ERR(page)) {
			pr_err("%s: err page for %08lx(%lu)\n", __func__, start,
					PTR_ERR(page));
			ret = PTR_ERR(page);
			goto out;
		}

		offset = start & ~PAGE_MASK;
		rest = min_t(ssize_t, PAGE_SIZE - offset, len);

		sg_set_page(&sg[i], page, rest, offset);

		len -= rest;
		start += rest;
		pg_i++, i++;
	}

	if (i != map_obj->dma_info.num_pages) {
		pr_err("%s: bad number of sg iterations\n", __func__);
		ret = -EFAULT;
		goto out;
	}

out:
	return ret;
}

static int memory_regain_ownership(struct device *dev,
		struct dmm_map_object *map_obj, unsigned long start,
		ssize_t len, enum dma_data_direction dir)
{
	int ret = 0;
	unsigned long first_data_page = start >> PAGE_SHIFT;
	unsigned long last_data_page = ((u32)(start + len - 1) >> PAGE_SHIFT);
	/* calculating the number of pages this area spans */
	unsigned long num_pages = last_data_page - first_data_page + 1;
	struct device_dma_map_info *dma_info = &map_obj->dma_info;

	if (!dma_info->sg)
		goto out;

	if (dma_info->dir != dir || dma_info->num_pages != num_pages) {
		pr_err("%s: dma info doesn't match given params\n", __func__);
		return -EINVAL;
	}

	dma_unmap_sg(dev, dma_info->sg, num_pages, dma_info->dir);

	pr_debug("%s: dma_map_sg unmapped\n", __func__);

	kfree(dma_info->sg);

	map_obj->dma_info.sg = NULL;

out:
	return ret;
}

/* Cache operation against kernel address instead of users */
static int memory_give_ownership(struct device *dev,
		struct dmm_map_object *map_obj, unsigned long start,
		ssize_t len, enum dma_data_direction dir)
{
	int pg_i, ret, sg_num;
	struct scatterlist *sg;
	unsigned long first_data_page = start >> PAGE_SHIFT;
	unsigned long last_data_page = ((u32)(start + len - 1) >> PAGE_SHIFT);
	/* calculating the number of pages this area spans */
	unsigned long num_pages = last_data_page - first_data_page + 1;

	pg_i = find_first_page_in_cache(map_obj, start);
	if (pg_i < 0) {
		pr_err("%s: failed to find first page in cache\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	sg = kcalloc(num_pages, sizeof(*sg), GFP_KERNEL);
	if (!sg) {
		pr_err("%s: kcalloc failed\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	sg_init_table(sg, num_pages);

	/* cleanup a previous sg allocation */
	/* this may happen if application doesn't signal for e/o DMA */
	kfree(map_obj->dma_info.sg);

	map_obj->dma_info.sg = sg;
	map_obj->dma_info.dir = dir;
	map_obj->dma_info.num_pages = num_pages;

	ret = build_dma_sg(map_obj, start, len, pg_i);
	if (ret)
		goto kfree_sg;

	sg_num = dma_map_sg(dev, sg, num_pages, dir);
	if (sg_num < 1) {
		pr_err("%s: dma_map_sg failed: %d\n", __func__, sg_num);
		ret = -EFAULT;
		goto kfree_sg;
	}

	pr_debug("%s: dma_map_sg mapped %d elements\n", __func__, sg_num);
	map_obj->dma_info.sg_num = sg_num;

	return 0;

kfree_sg:
	kfree(sg);
	map_obj->dma_info.sg = NULL;
out:
	return ret;
}
#endif

int proc_begin_dma(struct iodmm_struct *obj, void *pva, u32 ul_size,
					enum dma_data_direction dir)
{
	/* Keep STATUS here for future additions to this function */
	int status = 0;
	u32 end = (u32)pva + ul_size;
#if 0
	u32 va_align;
	struct dmm_map_object *map_obj;
	struct device *dev = obj->iovmm->iommu->dev;
	va_align = round_down((u32)pva, PAGE_SIZE);

	pr_debug("%s: addr 0x%x, size 0x%x, type %d\n", __func__,
							(u32)va_align,
							ul_size, dir);
	/* find requested memory are in cached mapping information */
	map_obj = find_containing_mapping(obj, (u32) va_align, 0, true,
								ul_size);
	if (!map_obj) {
		pr_err("%s: find_containing_mapping failed\n", __func__);
		status = -EFAULT;
		goto err_out;
	}

	if (memory_give_ownership(dev, map_obj, (u32) va_align, ul_size, dir)) {
		pr_err("%s: InValid address parameters %x %x\n",
			       __func__, va_align, ul_size);
		status = -EFAULT;
	}

err_out:
#endif
	status = temp_user_dma_op((u32)pva, end, 3);
	return status;
}

int proc_end_dma(struct iodmm_struct *obj, void *pva, u32 ul_size,
			enum dma_data_direction dir)
{
	/* Keep STATUS here for future additions to this function */
	int status = 0;
	u32 end = (u32)pva + ul_size;
#if 0
	u32 va_align;
	struct dmm_map_object *map_obj;
	struct device *dev = obj->iovmm->iommu->dev;
	va_align = round_down((u32)pva, PAGE_SIZE);

	pr_debug("%s: addr 0x%x, size 0x%x, type %d\n", __func__,
							(u32)va_align,
							ul_size, dir);

	/* find requested memory are in cached mapping information */
	map_obj = find_containing_mapping(obj, (u32) va_align, 0, true,
								ul_size);
	if (!map_obj) {
		pr_err("%s: find_containing_mapping failed\n", __func__);
		status = -EFAULT;
		goto err_out;
	}

	if (memory_regain_ownership(dev, map_obj, va_align, ul_size, dir)) {
		pr_err("%s: InValid address parameters %p %x\n",
		       __func__, pva, ul_size);
		status = -EFAULT;
		goto err_out;
	}

err_out:
#endif
	status = temp_user_dma_op((u32)pva, end, 1);
	return status;
}

/**
 * device_flush_memory - Flushes the memory specified
 * @obj:	target dmm object
 * @pva		User address to be flushed
 * @size	Size of the buffer to be flushed
 * Flushes the memory specified
 **/
int device_flush_memory(struct iodmm_struct *obj, void *pva,
			     u32 ul_size, u32 ul_flags)
{
	enum dma_data_direction dir = DMA_BIDIRECTIONAL;

	return proc_begin_dma(obj, pva, ul_size, dir);
}

/**
 * device_invalidate_memory - Invalidates the memory specified
 * @obj:	target dmm object
 * @pva		User address to be invalidated
 * @size	Size of the buffer to be invalidated
 * Invalidates the memory specified
 **/
int device_invalidate_memory(struct iodmm_struct *obj, void *pva, u32 size)
{
	enum dma_data_direction dir = DMA_FROM_DEVICE;

	return proc_begin_dma(obj, pva, size, dir);
}

/**
 * user_to_device_map() - maps user to dsp virtual address
 * @mmu:	Pointer to iommu handle.
 * @uva:		Virtual user space address.
 * @da		DSP address
 * @size		Buffer size to map.
 * @usr_pgs	struct page array pointer where the user pages will be stored
 *
 * This function maps a user space buffer into DSP virtual address.
 *
 */
int user_to_device_map(struct iommu *mmu, u32 uva, u32 da, u32 size,
						struct page **usr_pgs)

{
	int res = 0;
	int w;
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	u32 pg_num;
	u32 status;
	int pg_i;
	u32 pa;
	unsigned int pages;
	struct iotlb_entry tlb_entry;
	struct page *mapped_page;

	if (!size || !usr_pgs)
		return -EINVAL;

	pages = size / PAGE_SIZE;

	vma = find_vma(mm, uva);

	if (vma->vm_flags & (VM_WRITE | VM_MAYWRITE))
		w = 1;

	for (pg_i = 0; pg_i < pages; pg_i++) {
		pg_num = get_user_pages(current, mm, uva, 1,
						w, 1, &mapped_page, NULL);
		if (pg_num > 0) {
			if (page_count(mapped_page) < 1) {
				pr_err("Bad page count after doing"
					"get_user_pages on"
					"user buffer\n");
				break;
			}
			tlb_entry.pgsz = MMU_CAM_PGSZ_4K;
			tlb_entry.prsvd = 0;
			tlb_entry.valid = MMU_CAM_V;
			tlb_entry.elsz = MMU_RAM_ELSZ_32;
			tlb_entry.endian = MMU_RAM_ENDIAN_LITTLE;
			tlb_entry.mixed = MMU_RAM_MIXED;
			tlb_entry.da = da;
			pa = page_to_phys(mapped_page);
			tlb_entry.pa = (u32)pa;
			iopgtable_store_entry(mmu, &tlb_entry);
			if (usr_pgs)
				usr_pgs[pg_i] = mapped_page;
			da += PAGE_SIZE;
			uva += PAGE_SIZE;
		} else {
			pr_err("get_user_pages FAILED,"
				"MPU addr = 0x%x,"
				"vma->vm_flags = 0x%lx,"
				"get_user_pages Err"
				"Value = %d, Buffer"
				"size=0x%x\n", uva,
				vma->vm_flags, pg_num,
				size);
			status = -EFAULT;
			break;
		}
	}
	return res;
}

/**
 * phys_to_device_map() - maps physical addr
 * to device virtual address
 * @mmu:       Pointer to iommu handle.
 * @uva:               Virtual user space address.
 * @da         DSP address
 * @size               Buffer size to map.
 * @usr_pgs    struct page array pointer where the user pages will be stored
 *
 * This function maps a user space buffer into DSP virtual address.
 *
 */
int phys_to_device_map(struct iommu *mmu, u32 phys, u32 da, u32 size,
						struct page **usr_pgs)
{
	int res = 0;
	int pg_i;
	unsigned int pages;
	struct iotlb_entry tlb_entry;

	if (!size || !usr_pgs)
		return -EINVAL;

	pages = size / PAGE_SIZE;

	for (pg_i = 0; pg_i < pages; pg_i++) {
		tlb_entry.pgsz = MMU_CAM_PGSZ_4K;
		tlb_entry.prsvd = MMU_CAM_P;
		tlb_entry.valid = MMU_CAM_V;
		tlb_entry.elsz = MMU_RAM_ELSZ_8;
		tlb_entry.endian = MMU_RAM_ENDIAN_LITTLE;
		tlb_entry.mixed = 0;
		tlb_entry.da = da;
		tlb_entry.pa = (u32)phys;
		iopgtable_store_entry(mmu, &tlb_entry);
		da += PAGE_SIZE;
		phys += PAGE_SIZE;
	}

	return res;
}

/**
 * user_to_device_unmap() - unmaps Device virtual buffer.
 * @mmu:	Pointer to iommu handle.
 * @da		DSP address
 *
 * This function unmaps a user space buffer into DSP virtual address.
 *
 */
int user_to_device_unmap(struct iommu *mmu, u32 da, unsigned size)
{
	unsigned total = size;
	unsigned start = da;

	while (total > 0) {
		size_t bytes;
		bytes = iopgtable_clear_entry(mmu, start);
		if (bytes == 0)
			bytes = PAGE_SIZE;
		else
			dev_dbg(mmu->dev, "%s: unmap 0x%x 0x%x\n",
				__func__, start, bytes);
		BUG_ON(!IS_ALIGNED(bytes, PAGE_SIZE));
		total -= bytes;
		start += bytes;
	}
	return 0;
}

/*
 *  ======== user_va2_pa ========
 *  Purpose:
 *      This function walks through the page tables to convert a userland
 *      virtual address to physical address
 */
static u32 user_va2_pa(struct mm_struct *mm, u32 address)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *ptep, pte;

	pgd = pgd_offset(mm, address);
	if (!(pgd_none(*pgd) || pgd_bad(*pgd))) {
		pmd = pmd_offset(pgd, address);
		if (!(pmd_none(*pmd) || pmd_bad(*pmd))) {
			ptep = pte_offset_map(pmd, address);
			if (ptep) {
				pte = *ptep;
				if (pte_present(pte))
					return pte & PAGE_MASK;
			}
		}
	}
	return 0;
}

/**
 * dmm_user - Maps user buffer to Device address
 * @obj:	target dmm object
 * @pool_id:	DMM pool id
 * @da:		Mapped Device Address
 * @va:		User virtual Address
 * @bytes	Size of the buffer to be mapped
 * flags	flags on how to interpret user buffer
 *
 * Maps given user buffer to Device address
 **/
int dmm_user(struct iodmm_struct *obj, u32 pool_id, u32 *da,
				u32 va, size_t bytes, u32 flags)
{
	bool found = false;
	struct iovmm_pool *pool;
	struct gen_pool *gen_pool;
	struct dmm_map_object *dmm_obj;
	struct iovmm_device *iovmm_obj = obj->iovmm;
	u32 pa_align, da_align, size_align, tmp_addr;
	int err = 0;
	int i, num_of_pages;
	struct page *pg;
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;

	/*
	 * Important Note: va is mapped from user application process
	 * to current process - it must lie completely within the current
	 * virtual memory address space in order to be of use to us here!
	 */
	down_read(&mm->mmap_sem);

	/* Calculate the page-aligned PA, VA and size */
	pa_align = round_down((u32) va, PAGE_SIZE);
	size_align = round_up(bytes + va - pa_align, PAGE_SIZE);

	/*
	 * Hack hack for Tiler. Don't add mapping information
	 * for Tiler buffers since they are already pre-mapped
	 * in TLB
	 */
	if (flags == IOVMF_DA_PHYS) {
		da_align = user_va2_pa(current->mm, va);
		*da = (da_align | (va & (PAGE_SIZE - 1)));
		err = 0;
		goto err;
	}

	vma = find_vma(mm, va);
	if (vma) {
		dev_dbg(iovmm_obj->iommu->dev,
			"VMAfor UserBuf: ul_mpu_addr=%x, ul_num_bytes=%x, "
			"vm_start=%lx, vm_end=%lx, vm_flags=%lx\n", va,
			bytes, vma->vm_start, vma->vm_end,
			vma->vm_flags);
	}
	/*
	 * It is observed that under some circumstances, the user buffer is
	 * spread across several VMAs. So loop through and check if the entire
	 * user buffer is covered
	 */
	while ((vma) && (va + bytes > vma->vm_end)) {
		/* jump to the next VMA region */
		vma = find_vma(mm, vma->vm_end + 1);
		dev_dbg(iovmm_obj->iommu->dev,
			"VMA for UserBuf ul_mpu_addr=%x ul_num_bytes=%x, "
			"vm_start=%lx, vm_end=%lx, vm_flags=%lx\n", va,
			bytes, vma->vm_start, vma->vm_end,
			vma->vm_flags);
	}
	if (!vma) {
		pr_err("%s: Failed to get VMA region for 0x%x (%d)\n",
		       __func__, va, bytes);
		err = -EINVAL;
		goto err;
	}

	if (vma->vm_flags & VM_IO) {
		da_align = user_va2_pa(current->mm, va);
		*da = (da_align | (va & (PAGE_SIZE - 1)));
		dmm_obj = add_mapping_info(obj, NULL, va, da_align,
							size_align);
		if (dmm_obj == NULL) {
			err = -EINVAL;
			goto err;
		}
		num_of_pages = size_align/PAGE_SIZE;
		for (i = 0; i < num_of_pages; i++) {
			pg = phys_to_page(da_align);
			da_align += PAGE_SIZE;
			dmm_obj->pages[i] = pg;
		}
		err = 0;
		goto err;
	}

	list_for_each_entry(pool, &iovmm_obj->mmap_pool, list) {
		if (pool->pool_id == pool_id) {
			gen_pool = pool->genpool;
			found = true;
			break;
		}
	}
	if (found == false) {
		err = -EINVAL;
		goto err;
	}
	da_align = gen_pool_alloc(gen_pool, bytes);

	/* Mapped address = MSB of VA | LSB of PA */
	tmp_addr = (da_align | ((u32) va & (PAGE_SIZE - 1)));
	dmm_obj = add_mapping_info(obj, gen_pool, pa_align, tmp_addr,
							size_align);
	if (!dmm_obj)
		goto err;

	err = user_to_device_map(iovmm_obj->iommu, pa_align,
				da_align, size_align, dmm_obj->pages);
	if ((!err) && (flags & IOVMF_DA_USER))
		*da = tmp_addr;

err:
	up_read(&mm->mmap_sem);
	return err;
}

/**
 * user_un_map - Removes User's mapped address
 * @obj:	target dmm object
 * @map_addr	Mapped address that needs to be unmapped
 *
 * removes user's dmm buffer mapping
 **/
int user_un_map(struct iodmm_struct *obj, u32 map_addr)
{
	int status = 0;
	u32 va_align;
	u32 size_align;
	struct dmm_map_object *map_obj;
	int i;
	struct page *pg;

	va_align = round_down(map_addr, PAGE_SIZE);

	/*
	* Update DMM structures. Get the size to unmap.
	* This function returns error if the VA is not mapped
	*/
	/* find requested memory are in cached mapping information */
	map_obj = find_containing_mapping(obj, 0, map_addr, false, 0);
	if (!map_obj)
		goto err;
	size_align = map_obj->size;
	/* Remove mapping from the page tables. */
	status = user_to_device_unmap(obj->iovmm->iommu, va_align,
							size_align);
	if (status)
		goto err;

	i = size_align/PAGE_SIZE;
	while (i--) {
		pg = map_obj->pages[i];
		if (pfn_valid(page_to_pfn(pg))) {
			if (page_count(pg) < 1)
				pr_info("%s UNMAP FAILURE !!!\n", __func__);
			else {
				SetPageDirty(pg);
				page_cache_release(pg);
			}
		}
	}
	/*
	* A successful unmap should be followed by removal of map_obj
	* from dmm_map_list, so that mapped memory resource tracking
	* remains uptodate
	*/
	remove_mapping_information(obj, map_obj->da, map_obj->size);

	return 0;
err:
	return status;
}

/**
 * user_remove_resources - Removes User's dmm resources
 * @obj:	target dmm object
 *
 * removes user's dmm resources
 **/
void user_remove_resources(struct iodmm_struct *obj)
{

	struct dmm_map_object *temp_map, *map_obj;
	int status = 0;

	/* Free DMM mapped memory resources */
	list_for_each_entry_safe(map_obj, temp_map, &obj->map_list, link) {
		status = user_un_map(obj, map_obj->da);
		if (status) {
			pr_err("%s: proc_un_map failed!"
				" status = 0x%x\n", __func__, status);
		}
	}
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Userspace DMM to IOMMU");
MODULE_AUTHOR("Hari Kanigeri");
MODULE_AUTHOR("Ramesh Gupta");
MODULE_AUTHOR("Ohad Ben-Cohen");
#endif
