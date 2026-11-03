// Rust counterpart to c/mini_gc.h.
//
// Rust's standard library has no tracing collector. This module supplies the
// same precise mark-sweep algorithm used by the C and C++ tracks: a header with
// a trace callback, an all-objects list, a worklist mark phase, and a sweep that
// recycles unmarked objects into a size-uniform free list. Objects are
// bump-allocated from large blocks so allocation is not unfairly penalised by
// one system allocation per object.
use std::alloc::{alloc, dealloc, handle_alloc_error, Layout};
use std::ptr;

#[repr(C)]
pub struct Object {
    pub next_all: *mut Object,
    pub next_free: *mut Object,
    pub trace: Option<unsafe fn(*mut Object, &mut Vec<*mut Object>)>,
    pub object_bytes: u32,
    pub marked: u32,
}

struct Block {
    data: *mut u8,
    capacity: usize,
    used: usize,
    next: *mut Block,
}

pub struct MiniGc {
    blocks: *mut Block,
    all_objects: *mut Object,
    free_list: *mut Object,
    root: *mut Object,
    mark_stack: Vec<*mut Object>,
    live_objects: usize,
    collections: usize,
    total_allocations: usize,
    block_bytes: usize,
}

impl MiniGc {
    pub fn new(block_bytes: usize) -> Self {
        Self {
            blocks: ptr::null_mut(),
            all_objects: ptr::null_mut(),
            free_list: ptr::null_mut(),
            root: ptr::null_mut(),
            mark_stack: Vec::new(),
            live_objects: 0,
            collections: 0,
            total_allocations: 0,
            block_bytes: block_bytes.max(4096),
        }
    }

    pub fn set_root(&mut self, root: *mut Object) {
        self.root = root;
    }

    pub fn alloc(
        &mut self,
        bytes: usize,
        trace: Option<unsafe fn(*mut Object, &mut Vec<*mut Object>)>,
    ) -> *mut Object {
        let object: *mut Object;
        if !self.free_list.is_null() {
            object = self.free_list;
            self.free_list = unsafe { (*object).next_free };
        } else {
            let mut block = self.blocks;
            if block.is_null()
                || unsafe { (*block).used + bytes > (*block).capacity }
            {
                let capacity = self.block_bytes.max(bytes);
                let layout = Layout::from_size_align(capacity, 8).unwrap();
                let data = unsafe { alloc(layout) };
                if data.is_null() {
                    handle_alloc_error(layout);
                }
                let new_block = Box::into_raw(Box::new(Block {
                    data,
                    capacity,
                    used: 0,
                    next: self.blocks,
                }));
                self.blocks = new_block;
                block = new_block;
            }
            object =
                unsafe { (*block).data.add((*block).used) as *mut Object };
            unsafe {
                (*block).used += bytes;
            }
        }

        unsafe {
            ptr::write_bytes(object as *mut u8, 0, bytes);
            (*object).next_all = self.all_objects;
            self.all_objects = object;
            (*object).next_free = ptr::null_mut();
            (*object).trace = trace;
            (*object).object_bytes = bytes as u32;
            (*object).marked = 0;
        }
        self.live_objects += 1;
        self.total_allocations += 1;
        object
    }

    pub fn collect(&mut self) -> usize {
        self.mark_stack.clear();
        if !self.root.is_null() {
            self.mark_stack.push(self.root);
        }
        while let Some(object) = self.mark_stack.pop() {
            unsafe {
                if (*object).marked != 0 {
                    continue;
                }
                (*object).marked = 1;
                if let Some(trace) = (*object).trace {
                    trace(object, &mut self.mark_stack);
                }
            }
        }

        let mut link: *mut *mut Object = &mut self.all_objects;
        while !unsafe { *link }.is_null() {
            let object = unsafe { *link };
            unsafe {
                if (*object).marked != 0 {
                    (*object).marked = 0;
                    link = &mut (*object).next_all;
                } else {
                    *link = (*object).next_all;
                    (*object).next_free = self.free_list;
                    self.free_list = object;
                    self.live_objects -= 1;
                }
            }
        }

        self.collections += 1;
        self.live_objects
    }

    pub fn live_objects(&self) -> usize {
        self.live_objects
    }

    pub fn collections(&self) -> usize {
        self.collections
    }
}

impl Drop for MiniGc {
    fn drop(&mut self) {
        let mut block = self.blocks;
        while !block.is_null() {
            unsafe {
                let next = (*block).next;
                let layout =
                    Layout::from_size_align((*block).capacity, 8).unwrap();
                dealloc((*block).data, layout);
                drop(Box::from_raw(block));
                block = next;
            }
        }
    }
}
