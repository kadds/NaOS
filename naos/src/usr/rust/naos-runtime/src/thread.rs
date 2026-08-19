use alloc::boxed::Box;
use core::mem::size_of;
use core::ptr;
use core::sync::atomic::{
    AtomicBool, AtomicI32, AtomicI64, AtomicPtr, AtomicU32, AtomicU64, Ordering,
};

use naos_sys as sys;

const PAGE_SIZE: usize = 4096;
const TLS_SLOT_COUNT: usize = 32;
const TLS_SLOT_SIZE: usize = 16;
const TLS_AREA_SIZE: usize = TLS_SLOT_COUNT * TLS_SLOT_SIZE;
const FUTEX_WAIT: i32 = 2;
const FUTEX_WAKE: i32 = 1;
const PT_LOAD: u32 = 1;
const PT_TLS: u32 = 7;

#[repr(C)]
#[derive(Clone, Copy)]
struct Elf64ProgramHeader {
    p_type: u32,
    p_flags: u32,
    p_offset: u64,
    p_vaddr: u64,
    p_paddr: u64,
    p_filesz: u64,
    p_memsz: u64,
    p_align: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TlsTemplate {
    image: *const u8,
    filesz: usize,
    memsz: usize,
    align: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TlsTcbPrefix {
    pub self_pointer: *mut u8,
    pub dtv_size: u64,
    pub dtv_pointer: *mut *mut u8,
    pub tid: u32,
    pub did_exit: u32,
    pub reserved0: u64,
    pub stack_canary: u64,
    pub cancel_bits: u32,
    pub reserved1: u32,
}

#[repr(C, align(16))]
struct ThreadControlBlock {
    prefix: TlsTcbPrefix,
    allocation_base: *mut u8,
    allocation_size: usize,
    tls_start: *mut u8,
    tls_size: usize,
    initialized_slots: AtomicU64,
    fixed_slots: [u8; TLS_AREA_SIZE],
    child_state: *mut JoinState,
    child_entry: Option<extern "C" fn(*mut u8) -> i64>,
    child_argument: *mut u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TlsError {
    InvalidLayout,
    AllocationFailed,
    SetFailed(i32),
}

#[derive(Clone, Copy)]
pub struct ThreadLocal<T: Copy> {
    slot: usize,
    marker: core::marker::PhantomData<T>,
}

impl<T: Copy> ThreadLocal<T> {
    pub const fn new(slot: usize) -> Self {
        Self {
            slot,
            marker: core::marker::PhantomData,
        }
    }

    pub fn get(&self) -> Option<T> {
        let tcb = current_tcb()?;
        if self.slot >= TLS_SLOT_COUNT {
            return None;
        }
        let mask = 1_u64 << self.slot;
        if unsafe { (*tcb).initialized_slots.load(Ordering::Acquire) & mask } == 0 {
            return None;
        }
        let address = slot_address(tcb, self.slot, size_of::<T>(), core::mem::align_of::<T>())?;
        Some(unsafe { ptr::read(address as *const T) })
    }

    pub fn set(&self, value: T) -> bool {
        let Some(tcb) = current_tcb() else {
            return false;
        };
        let Some(address) =
            slot_address(tcb, self.slot, size_of::<T>(), core::mem::align_of::<T>())
        else {
            return false;
        };
        unsafe { ptr::write(address as *mut T, value) };
        let mask = 1_u64 << self.slot;
        unsafe {
            (*tcb).initialized_slots.fetch_or(mask, Ordering::Release);
        }
        true
    }
}

static MAIN_TCB: AtomicPtr<ThreadControlBlock> = AtomicPtr::new(ptr::null_mut());
static TLS_TEMPLATE_PRESENT: AtomicBool = AtomicBool::new(false);
static TLS_TEMPLATE_IMAGE: AtomicU64 = AtomicU64::new(0);
static TLS_TEMPLATE_FILE_SIZE: AtomicU64 = AtomicU64::new(0);
static TLS_TEMPLATE_MEMORY_SIZE: AtomicU64 = AtomicU64::new(0);
static TLS_TEMPLATE_ALIGN: AtomicU64 = AtomicU64::new(1);

/// Find and validate the single static PT_TLS module described by the ELF
/// program headers. The loader has already mapped PT_LOAD segments, so the
/// returned image pointer is a read-only address in the current executable.
pub unsafe fn discover_static_tls(
    program_headers: usize,
    program_header_size: usize,
    program_header_count: usize,
) -> Result<Option<TlsTemplate>, TlsError> {
    if program_headers == 0
        || program_header_size < size_of::<Elf64ProgramHeader>()
        || program_header_count == 0
        || program_header_count > 128
    {
        return Err(TlsError::InvalidLayout);
    }

    let mut tls = None;
    for index in 0..program_header_count {
        let offset = index
            .checked_mul(program_header_size)
            .and_then(|value| program_headers.checked_add(value))
            .ok_or(TlsError::InvalidLayout)?;
        let header = unsafe { ptr::read_unaligned(offset as *const Elf64ProgramHeader) };
        if header.p_type != PT_TLS {
            continue;
        }
        if tls.is_some()
            || header.p_filesz > header.p_memsz
            || header.p_memsz as usize > sys::TLS_MAX_SIZE
            || header.p_align == 0
            || !header.p_align.is_power_of_two()
            || header.p_align as usize > sys::TLS_MAX_ALIGN
            || header.p_vaddr > usize::MAX as u64
            || header.p_filesz > usize::MAX as u64
            || header.p_memsz > usize::MAX as u64
        {
            return Err(TlsError::InvalidLayout);
        }
        tls = Some(TlsTemplate {
            image: header.p_vaddr as *const u8,
            filesz: header.p_filesz as usize,
            memsz: header.p_memsz as usize,
            align: header.p_align as usize,
        });
    }

    let Some(template) = tls else {
        return Ok(None);
    };
    let template_start = template.image as usize;
    let template_end = template_start
        .checked_add(template.filesz)
        .ok_or(TlsError::InvalidLayout)?;
    let mut contained = false;
    for index in 0..program_header_count {
        let offset = index
            .checked_mul(program_header_size)
            .and_then(|value| program_headers.checked_add(value))
            .ok_or(TlsError::InvalidLayout)?;
        let header = unsafe { ptr::read_unaligned(offset as *const Elf64ProgramHeader) };
        if header.p_type != PT_LOAD {
            continue;
        }
        if header.p_vaddr > usize::MAX as u64
            || header.p_filesz > usize::MAX as u64
            || header.p_memsz > usize::MAX as u64
        {
            return Err(TlsError::InvalidLayout);
        }
        let load_start = header.p_vaddr as usize;
        let load_end = load_start
            .checked_add(header.p_filesz as usize)
            .ok_or(TlsError::InvalidLayout)?;
        if template_start >= load_start && template_end <= load_end {
            contained = true;
            break;
        }
    }
    if !contained && template.filesz != 0 {
        return Err(TlsError::InvalidLayout);
    }
    Ok(Some(template))
}

pub fn initialize_main_tls(template: Option<TlsTemplate>) -> Result<(), TlsError> {
    if !MAIN_TCB.load(Ordering::Acquire).is_null() {
        return Ok(());
    }
    publish_template(template);
    let tcb = allocate_tcb(template)?;
    set_thread_metadata(tcb);
    let status = unsafe { sys::_s_tcb_set(tcb as *mut u8) };
    if status != 0 {
        unsafe { free_tcb(tcb) };
        return Err(TlsError::SetFailed(status));
    }
    MAIN_TCB.store(tcb, Ordering::Release);
    Ok(())
}

pub struct JoinHandle {
    state: *mut JoinState,
}

unsafe impl Send for JoinHandle {}

impl JoinHandle {
    pub fn join(mut self) -> Result<i64, ThreadError> {
        if self.state.is_null() {
            return Err(ThreadError::AlreadyJoined);
        }
        let state = unsafe { &*self.state };
        while state.done.load(Ordering::Acquire) == 0 {
            let _ = unsafe {
                sys::_s_futex(
                    &state.done as *const AtomicI32 as *mut i32,
                    FUTEX_WAIT,
                    0,
                    ptr::null(),
                )
            };
        }
        let result = state.result.load(Ordering::Acquire);
        let state_pointer = self.state;
        self.state = ptr::null_mut();
        unsafe { release_state(state_pointer) };
        Ok(result)
    }
}

impl Drop for JoinHandle {
    fn drop(&mut self) {
        if !self.state.is_null() {
            unsafe { release_state(self.state) };
            self.state = ptr::null_mut();
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ThreadError {
    AlreadyJoined,
    AllocationFailed,
    CloneFailed(i32),
    Tls(TlsError),
}

pub fn spawn(
    entry: extern "C" fn(*mut u8) -> i64,
    argument: *mut u8,
) -> Result<JoinHandle, ThreadError> {
    let tcb = allocate_tcb(load_template()).map_err(ThreadError::Tls)?;
    let state = Box::into_raw(Box::new(JoinState::new()));
    unsafe {
        (*tcb).child_state = state;
        (*tcb).child_entry = Some(entry);
        (*tcb).child_argument = argument;
    }
    let result = unsafe { sys::_s_clone(thread_entry as *mut u8, tcb as *mut u8, tcb as *mut u8) };
    if result < 0 {
        unsafe {
            free_tcb(tcb);
            release_state(state);
            release_state(state);
        }
        return Err(ThreadError::CloneFailed(result));
    }
    Ok(JoinHandle { state })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_thread_spawn(
    entry: extern "C" fn(*mut u8) -> i64,
    argument: *mut u8,
    output: *mut *mut u8,
) -> i32 {
    if output.is_null() {
        return -1;
    }
    match spawn(entry, argument) {
        Ok(handle) => {
            unsafe { output.write(Box::into_raw(Box::new(handle)) as *mut u8) };
            0
        }
        Err(error) => {
            let message: &'static [u8] = match error {
                ThreadError::Tls(_) => b"naos-runtime: std thread TLS allocation failed\0",
                ThreadError::CloneFailed(_) => b"naos-runtime: std thread clone failed\0",
                ThreadError::AllocationFailed => {
                    b"naos-runtime: std thread state allocation failed\0"
                }
                ThreadError::AlreadyJoined => b"naos-runtime: std thread invalid join state\0",
            };
            unsafe { sys::_s_log(message.as_ptr()) };
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_thread_join(handle: *mut u8) -> i64 {
    if handle.is_null() {
        return -1;
    }
    let handle = unsafe { Box::from_raw(handle as *mut JoinHandle) };
    handle.join().unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __naos_runtime_thread_detach(handle: *mut u8) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle as *mut JoinHandle)) };
    }
}

#[repr(C)]
struct JoinState {
    refs: AtomicU32,
    done: AtomicI32,
    result: AtomicI64,
}

impl JoinState {
    fn new() -> Self {
        Self {
            refs: AtomicU32::new(2),
            done: AtomicI32::new(0),
            result: AtomicI64::new(0),
        }
    }
}

unsafe extern "C" fn thread_entry(raw: *mut u8) -> ! {
    let tcb = raw as *mut ThreadControlBlock;
    set_thread_metadata(tcb);
    let (state, entry, argument) = unsafe {
        let tcb_ref = &*tcb;
        let Some(entry) = tcb_ref.child_entry else {
            sys::_s_exit_thread(-1);
        };
        (tcb_ref.child_state, entry, tcb_ref.child_argument)
    };
    let result = if current_tcb() == Some(tcb) {
        entry(argument)
    } else {
        -1
    };
    let state_ref = unsafe { &*state };
    unsafe {
        (&*((&(*tcb).prefix.did_exit) as *const u32 as *const AtomicU32))
            .store(1, Ordering::Release);
    }
    state_ref.result.store(result, Ordering::Release);
    state_ref.done.store(1, Ordering::Release);
    let _ = unsafe {
        sys::_s_futex(
            &state_ref.done as *const AtomicI32 as *mut i32,
            FUTEX_WAKE,
            i32::MAX,
            ptr::null(),
        )
    };
    let (allocation_base, allocation_size) = unsafe { tcb_allocation(tcb) };
    unsafe { release_state(state) };
    unsafe {
        if !allocation_base.is_null() && allocation_size != 0 {
            unmap(allocation_base, allocation_size);
        }
        sys::_s_exit_thread(result);
    }
}

fn current_tcb() -> Option<*mut ThreadControlBlock> {
    let value: usize;
    unsafe {
        core::arch::asm!(
            "movq %fs:0, {0}",
            out(reg) value,
            options(att_syntax, nostack, preserves_flags)
        );
    }
    (!value.eq(&0)).then_some(value as *mut ThreadControlBlock)
}

fn set_thread_metadata(tcb: *mut ThreadControlBlock) {
    let tid = unsafe { sys::_s_current_tid() };
    let tid = u32::try_from(tid).unwrap_or(0);
    unsafe {
        (&*((&(*tcb).prefix.tid) as *const u32 as *const AtomicU32)).store(tid, Ordering::Release);
        (&*((&(*tcb).prefix.did_exit) as *const u32 as *const AtomicU32))
            .store(0, Ordering::Release);
    }
}

fn slot_address(
    tcb: *mut ThreadControlBlock,
    slot: usize,
    size: usize,
    alignment: usize,
) -> Option<*mut u8> {
    if slot >= TLS_SLOT_COUNT || size > TLS_SLOT_SIZE || alignment == 0 || alignment > TLS_SLOT_SIZE
    {
        return None;
    }
    let address = unsafe { (*tcb).fixed_slots.as_ptr().add(slot * TLS_SLOT_SIZE) as *mut u8 };
    if (address as usize) % alignment != 0 {
        return None;
    }
    Some(address)
}

fn publish_template(template: Option<TlsTemplate>) {
    if let Some(template) = template {
        TLS_TEMPLATE_IMAGE.store(template.image as u64, Ordering::Relaxed);
        TLS_TEMPLATE_FILE_SIZE.store(template.filesz as u64, Ordering::Relaxed);
        TLS_TEMPLATE_MEMORY_SIZE.store(template.memsz as u64, Ordering::Relaxed);
        TLS_TEMPLATE_ALIGN.store(template.align as u64, Ordering::Relaxed);
        TLS_TEMPLATE_PRESENT.store(true, Ordering::Release);
    } else {
        TLS_TEMPLATE_PRESENT.store(false, Ordering::Release);
    }
}

fn load_template() -> Option<TlsTemplate> {
    if !TLS_TEMPLATE_PRESENT.load(Ordering::Acquire) {
        return None;
    }
    Some(TlsTemplate {
        image: TLS_TEMPLATE_IMAGE.load(Ordering::Relaxed) as *const u8,
        filesz: TLS_TEMPLATE_FILE_SIZE.load(Ordering::Relaxed) as usize,
        memsz: TLS_TEMPLATE_MEMORY_SIZE.load(Ordering::Relaxed) as usize,
        align: TLS_TEMPLATE_ALIGN.load(Ordering::Relaxed) as usize,
    })
}

fn allocate_tcb(template: Option<TlsTemplate>) -> Result<*mut ThreadControlBlock, TlsError> {
    let template = template.unwrap_or(TlsTemplate {
        image: ptr::null(),
        filesz: 0,
        memsz: 0,
        align: 1,
    });
    if (template.filesz != 0 && template.image.is_null())
        || template.filesz > template.memsz
        || template.memsz > sys::TLS_MAX_SIZE
        || template.align == 0
        || !template.align.is_power_of_two()
        || template.align > sys::TLS_MAX_ALIGN
    {
        return Err(TlsError::InvalidLayout);
    }
    let image_alignment = template.align;
    let tcb_alignment = image_alignment.max(core::mem::align_of::<ThreadControlBlock>());
    let tls_size = round_up(template.memsz, image_alignment).ok_or(TlsError::InvalidLayout)?;
    let total = tcb_alignment
        .checked_sub(1)
        .and_then(|padding| tls_size.checked_add(size_of::<ThreadControlBlock>() + padding))
        .ok_or(TlsError::InvalidLayout)?;
    let map_len = round_up(total, PAGE_SIZE).ok_or(TlsError::InvalidLayout)?;
    if map_len as u64 > sys::MEMORY_MAP_MAX_BYTES {
        return Err(TlsError::InvalidLayout);
    }

    let mut frame = sys::MemoryMapFrame {
        struct_size: size_of::<sys::MemoryMapFrame>() as u32,
        flags: sys::MEMORY_MAP_READ | sys::MEMORY_MAP_WRITE,
        length: map_len as u64,
        ..sys::MemoryMapFrame::default()
    };
    let status = unsafe { sys::_na_memory_map(&mut frame) };
    if status != sys::STATUS_OK || frame.address == 0 {
        return Err(TlsError::AllocationFailed);
    }
    let base = frame.address as usize;
    let Some((tcb_address, image_start)) = tcb_layout(base, tls_size, map_len, tcb_alignment)
    else {
        unsafe { unmap(frame.address as *mut u8, map_len) };
        return Err(TlsError::InvalidLayout);
    };

    unsafe {
        if template.filesz != 0 {
            ptr::copy_nonoverlapping(template.image, image_start as *mut u8, template.filesz);
        }
        if template.memsz > template.filesz {
            ptr::write_bytes(
                (image_start + template.filesz) as *mut u8,
                0,
                template.memsz - template.filesz,
            );
        }
        ptr::write(
            tcb_address as *mut ThreadControlBlock,
            ThreadControlBlock {
                prefix: TlsTcbPrefix {
                    self_pointer: tcb_address as *mut u8,
                    dtv_size: 1,
                    dtv_pointer: ptr::null_mut(),
                    tid: 0,
                    did_exit: 0,
                    reserved0: 0,
                    stack_canary: 0,
                    cancel_bits: 0,
                    reserved1: 0,
                },
                allocation_base: frame.address as *mut u8,
                allocation_size: map_len,
                tls_start: image_start as *mut u8,
                tls_size: template.memsz,
                initialized_slots: AtomicU64::new(0),
                fixed_slots: [0; TLS_AREA_SIZE],
                child_state: ptr::null_mut(),
                child_entry: None,
                child_argument: ptr::null_mut(),
            },
        );
    }
    Ok(tcb_address as *mut ThreadControlBlock)
}

unsafe fn free_tcb(tcb: *mut ThreadControlBlock) {
    if tcb.is_null() {
        return;
    }
    let base = unsafe { (*tcb).allocation_base };
    let size = unsafe { (*tcb).allocation_size };
    if !base.is_null() && size != 0 {
        unsafe { unmap(base, size) };
    }
}

unsafe fn tcb_allocation(tcb: *mut ThreadControlBlock) -> (*mut u8, usize) {
    if tcb.is_null() {
        return (ptr::null_mut(), 0);
    }
    unsafe { ((*tcb).allocation_base, (*tcb).allocation_size) }
}

unsafe fn unmap(address: *mut u8, length: usize) {
    let mut frame = sys::MemoryUnmapFrame {
        struct_size: size_of::<sys::MemoryUnmapFrame>() as u32,
        address: address as u64,
        length: length as u64,
        ..sys::MemoryUnmapFrame::default()
    };
    let _ = unsafe { sys::_na_memory_unmap(&mut frame) };
}

fn align_up(value: usize, alignment: usize) -> Option<usize> {
    let mask = alignment.checked_sub(1)?;
    value.checked_add(mask).map(|value| value & !mask)
}

fn round_up(value: usize, alignment: usize) -> Option<usize> {
    align_up(value, alignment)
}

fn tcb_layout(
    base: usize,
    tls_size: usize,
    map_len: usize,
    tcb_alignment: usize,
) -> Option<(usize, usize)> {
    let tcb_address = base
        .checked_add(tls_size)
        .and_then(|value| align_up(value, tcb_alignment))?;
    let image_start = tcb_address.checked_sub(tls_size)?;
    let map_end = base.checked_add(map_len)?;
    let tcb_end = tcb_address.checked_add(size_of::<ThreadControlBlock>())?;
    if tcb_end > map_end || tcb_address % core::mem::align_of::<ThreadControlBlock>() != 0 {
        return None;
    }
    Some((tcb_address, image_start))
}

unsafe fn release_state(state: *mut JoinState) {
    if unsafe { (*state).refs.fetch_sub(1, Ordering::AcqRel) } == 1 {
        unsafe { drop(Box::from_raw(state)) };
    }
}

#[cfg(test)]
mod tests {
    use super::{
        Elf64ProgramHeader, JoinState, TLS_AREA_SIZE, TLS_SLOT_COUNT, TLS_SLOT_SIZE,
        ThreadControlBlock, TlsError, TlsTcbPrefix, discover_static_tls, release_state,
        slot_address, tcb_layout,
    };
    use core::mem::{align_of, size_of};
    use core::sync::atomic::Ordering;

    fn valid_program_headers() -> [Elf64ProgramHeader; 2] {
        [
            Elf64ProgramHeader {
                p_type: super::PT_LOAD,
                p_flags: 5,
                p_offset: 0,
                p_vaddr: 0x400000,
                p_paddr: 0,
                p_filesz: 0x1000,
                p_memsz: 0x1000,
                p_align: 0x1000,
            },
            Elf64ProgramHeader {
                p_type: super::PT_TLS,
                p_flags: 6,
                p_offset: 0x800,
                p_vaddr: 0x400800,
                p_paddr: 0,
                p_filesz: 8,
                p_memsz: 16,
                p_align: 8,
            },
        ]
    }

    unsafe fn discover(
        headers: &[Elf64ProgramHeader],
    ) -> Result<Option<super::TlsTemplate>, TlsError> {
        unsafe {
            discover_static_tls(
                headers.as_ptr() as usize,
                size_of::<Elf64ProgramHeader>(),
                headers.len(),
            )
        }
    }

    #[test]
    fn variant_two_slots_are_in_the_tcb_and_bounded() {
        let storage = core::mem::MaybeUninit::<ThreadControlBlock>::uninit();
        let tcb = storage.as_ptr() as *mut ThreadControlBlock;
        assert_eq!(TLS_AREA_SIZE, TLS_SLOT_COUNT * TLS_SLOT_SIZE);
        assert_eq!(align_of::<ThreadControlBlock>(), 16);
        assert_eq!(size_of::<ThreadControlBlock>() % 16, 0);
        assert_eq!(size_of::<TlsTcbPrefix>(), 0x38);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, self_pointer), 0x00);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, dtv_size), 0x08);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, dtv_pointer), 0x10);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, tid), 0x18);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, did_exit), 0x1c);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, stack_canary), 0x28);
        assert_eq!(core::mem::offset_of!(TlsTcbPrefix, cancel_bits), 0x30);
        assert_eq!(core::mem::offset_of!(ThreadControlBlock, prefix), 0x00);
        assert_eq!(super::sys::TLS_ABI_VERSION, 1);
        let first = slot_address(tcb, 0, 8, 8).unwrap();
        let last = slot_address(tcb, TLS_SLOT_COUNT - 1, 16, 16).unwrap();
        assert_eq!(
            last as usize - first as usize,
            (TLS_SLOT_COUNT - 1) * TLS_SLOT_SIZE
        );
        assert!(slot_address(tcb, TLS_SLOT_COUNT, 8, 8).is_none());
        assert!(slot_address(tcb, 0, TLS_SLOT_SIZE + 1, 8).is_none());
    }

    #[test]
    fn rejects_overflow_after_tls_mapping_and_preserves_variant_two_layout() {
        assert!(tcb_layout(usize::MAX - 7, 16, 4096, 16).is_none());
        let (tcb, image) = tcb_layout(0x100000, 32, 4096, 16).unwrap();
        assert_eq!(tcb, 0x100020);
        assert_eq!(image, 0x100000);
    }

    #[test]
    fn join_state_keeps_worker_reference_after_handle_drop() {
        let state = alloc::boxed::Box::into_raw(alloc::boxed::Box::new(JoinState::new()));
        assert_eq!(unsafe { (*state).refs.load(Ordering::Acquire) }, 2);
        unsafe { release_state(state) };
        assert_eq!(unsafe { (*state).refs.load(Ordering::Acquire) }, 1);
        unsafe { release_state(state) };
    }

    #[test]
    fn discovers_and_rejects_static_tls_program_headers() {
        let headers = valid_program_headers();
        let result = unsafe { discover(&headers) }.unwrap().unwrap();
        assert_eq!(result.filesz, 8);
        assert_eq!(result.memsz, 16);
        assert_eq!(result.align, 8);

        let mut malformed = headers;
        malformed[1].p_filesz = 17;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );
    }

    #[test]
    fn rejects_duplicate_bad_sized_and_misaligned_tls_templates() {
        let valid = valid_program_headers();

        let duplicate = [valid[0], valid[1], valid[1]];
        assert_eq!(
            unsafe { discover(&duplicate) },
            Err(TlsError::InvalidLayout)
        );

        let mut malformed = valid;
        malformed[1].p_align = 0;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );

        malformed[1].p_align = 3;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );

        malformed[1].p_align = 1 << 21;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );

        malformed[1].p_align = 8;
        malformed[1].p_memsz = super::sys::TLS_MAX_SIZE as u64 + 1;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );
    }

    #[test]
    fn rejects_tls_template_outside_load_and_address_overflow() {
        let valid = valid_program_headers();
        let mut malformed = valid;

        malformed[1].p_vaddr = 0x500000;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );

        malformed[1].p_vaddr = u64::MAX;
        assert_eq!(
            unsafe { discover(&malformed) },
            Err(TlsError::InvalidLayout)
        );

        assert_eq!(
            unsafe {
                discover_static_tls(
                    valid.as_ptr() as usize,
                    size_of::<Elf64ProgramHeader>() - 1,
                    valid.len(),
                )
            },
            Err(TlsError::InvalidLayout)
        );
        assert_eq!(
            unsafe {
                discover_static_tls(valid.as_ptr() as usize, size_of::<Elf64ProgramHeader>(), 0)
            },
            Err(TlsError::InvalidLayout)
        );
    }
}
