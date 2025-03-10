/*
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "gl_heap.h"

/*
================================================================================

	DEVICE MEMORY HEAP
	Dumbest possible allocator for device memory.

================================================================================
*/

/*
===============
GL_CreateHeap
===============
*/
glheap_t *GL_CreateHeap (VkDeviceSize size, uint32_t memory_type_index, vulkan_memory_type_t memory_type, const char *name)
{
	glheap_t *heap = (glheap_t *)Mem_Alloc (sizeof (glheap_t));

	VkMemoryAllocateInfo memory_allocate_info;
	memset (&memory_allocate_info, 0, sizeof (memory_allocate_info));
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.allocationSize = size;
	memory_allocate_info.memoryTypeIndex = memory_type_index;

	R_AllocateVulkanMemory (&heap->memory, &memory_allocate_info, memory_type);
	GL_SetObjectName ((uint64_t)heap->memory.handle, VK_OBJECT_TYPE_DEVICE_MEMORY, name);

	heap->head = (glheapnode_t *)Mem_Alloc (sizeof (glheapnode_t));
	heap->head->offset = 0;
	heap->head->size = size;
	heap->head->prev = NULL;
	heap->head->next = NULL;
	heap->head->free = true;

	return heap;
}

/*
===============
GL_DestroyHeap
===============
*/
void GL_DestroyHeap (glheap_t *heap)
{
	GL_WaitForDeviceIdle ();
	R_FreeVulkanMemory (&heap->memory);
	Mem_Free (heap->head);
	Mem_Free (heap);
}

/*
===============
GL_HeapAllocate
===============
*/
glheapnode_t *GL_HeapAllocate (glheap_t *heap, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize *aligned_offset)
{
	glheapnode_t *current_node;
	glheapnode_t *best_fit_node = NULL;
	VkDeviceSize  best_fit_size = UINT64_MAX;

	for (current_node = heap->head; current_node != NULL; current_node = current_node->next)
	{
		if (!current_node->free)
			continue;

		const VkDeviceSize align_mod = current_node->offset % alignment;
		VkDeviceSize	   align_padding = (align_mod == 0) ? 0 : (alignment - align_mod);
		VkDeviceSize	   aligned_size = size + align_padding;

		if (current_node->size == aligned_size)
		{
			current_node->free = false;
			*aligned_offset = current_node->offset + align_padding;
			return current_node;
		}
		else if ((current_node->size > aligned_size) && (current_node->size < best_fit_size))
		{
			best_fit_size = current_node->size;
			best_fit_node = current_node;
		}
	}

	if (best_fit_node != NULL)
	{
		const VkDeviceSize align_mod = best_fit_node->offset % alignment;
		VkDeviceSize	   align_padding = (align_mod == 0) ? 0 : (alignment - align_mod);
		VkDeviceSize	   aligned_size = size + align_padding;

		glheapnode_t *new_node = (glheapnode_t *)Mem_Alloc (sizeof (glheapnode_t));
		*new_node = *best_fit_node;
		new_node->prev = best_fit_node->prev;
		new_node->next = best_fit_node;
		if (best_fit_node->prev)
			best_fit_node->prev->next = new_node;
		best_fit_node->prev = new_node;
		new_node->free = false;

		new_node->size = aligned_size;
		best_fit_node->size -= aligned_size;
		best_fit_node->offset += aligned_size;

		if (best_fit_node == heap->head)
			heap->head = new_node;

		*aligned_offset = new_node->offset + align_padding;
		return new_node;
	}

	*aligned_offset = 0;
	return NULL;
}

/*
===============
GL_HeapFree
===============
*/
void GL_HeapFree (glheap_t *heap, glheapnode_t *node)
{
	if (node->free)
		Sys_Error ("Trying to free a node that is already freed");

	node->free = true;
	if (node->prev && node->prev->free)
	{
		glheapnode_t *prev = node->prev;

		prev->next = node->next;
		if (node->next)
			node->next->prev = prev;

		prev->size += node->size;

		Mem_Free (node);
		node = prev;
	}

	if (node->next && node->next->free)
	{
		glheapnode_t *next = node->next;

		if (next->next)
			next->next->prev = node;
		node->next = next->next;

		node->size += next->size;

		Mem_Free (next);
	}
}

/*
===============
GL_IsHeapEmpty
===============
*/
qboolean GL_IsHeapEmpty (glheap_t *heap)
{
	return heap->head->next == NULL;
}

/*
================
GL_AllocateFromHeaps
================
*/
VkDeviceSize GL_AllocateFromHeaps (
	int *num_heaps, glheap_t ***heaps, VkDeviceSize heap_size, uint32_t memory_type_index, vulkan_memory_type_t memory_type, VkDeviceSize size,
	VkDeviceSize alignment, glheap_t **heap, glheapnode_t **heap_node, atomic_uint32_t *num_allocations, const char *heap_name)
{
	int i;
	int num_heaps_allocated = *num_heaps;

	for (i = 0; i < (num_heaps_allocated + 1); ++i)
	{
		if (i == num_heaps_allocated)
		{
			*heaps = Mem_Realloc (*heaps, sizeof (glheap_t *) * (num_heaps_allocated + 1));
			(*heaps)[i] = NULL;
			*num_heaps = num_heaps_allocated + 1;
		}

		qboolean new_heap = false;
		if (!(*heaps)[i])
		{
			(*heaps)[i] = GL_CreateHeap (heap_size, memory_type_index, memory_type, heap_name);
			Atomic_IncrementUInt32 (num_allocations);
			new_heap = true;
		}

		VkDeviceSize  aligned_offset;
		glheapnode_t *node = GL_HeapAllocate ((*heaps)[i], size, alignment, &aligned_offset);
		if (node)
		{
			*heap_node = node;
			*heap = (*heaps)[i];
			return aligned_offset;
		}
		else if (new_heap)
			break;
	}

	Sys_Error ("Could not allocate memory in '%s' heap", heap_name);
	return 0;
}

/*
================
GL_FreeFromHeaps
================
*/
void GL_FreeFromHeaps (int num_heaps, glheap_t **heaps, glheap_t *heap, glheapnode_t *heap_node, atomic_uint32_t *num_allocations)
{
	int i;
	GL_HeapFree (heap, heap_node);
	if (GL_IsHeapEmpty (heap))
	{
		Atomic_DecrementUInt32 (num_allocations);
		GL_DestroyHeap (heap);
		for (i = 0; i < num_heaps; ++i)
			if (heaps[i] == heap)
				heaps[i] = NULL;
	}
}
