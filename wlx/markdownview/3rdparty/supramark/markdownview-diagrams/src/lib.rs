//! One C ABI over supramark's `mermaid-little` and `plantuml-little`.
//!
//! Both renderers take `&str` source and return SVG, so the surface here is
//! tiny: render, free, and install the text-measurement callback the two
//! crates need before they can lay anything out.
//!
//! Symbols are prefixed `markdownview_diagrams_` rather than reusing
//! upstream's `supramark_*` names, so it is unambiguous at the link line
//! that this archive — not one of the per-renderer ones — is what got
//! linked. See Cargo.toml for why those cannot both be linked at once.

use std::os::raw::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};

pub const MDV_DIAGRAM_OK: c_int = 0;
pub const MDV_DIAGRAM_ERR_PARSE: c_int = 1;
pub const MDV_DIAGRAM_ERR_RENDER: c_int = 2;
pub const MDV_DIAGRAM_ERR_NULL_INPUT: c_int = 3;

/// Borrows the caller's bytes as UTF-8. A null pointer with a zero length
/// is the empty string; a null pointer with a non-zero length is a caller
/// bug and reports as null input rather than dereferencing.
///
/// # Safety
/// `ptr` must be valid for `len` bytes when `len != 0`.
unsafe fn input_to_str<'a>(ptr: *const u8, len: usize) -> Option<&'a str> {
    if ptr.is_null() {
        return if len == 0 { Some("") } else { None };
    }
    std::str::from_utf8(std::slice::from_raw_parts(ptr, len)).ok()
}

/// Hands an owned String to the caller as (ptr, len). Ownership moves to
/// the caller, who must return it through the matching free below.
fn deliver(svg: String, out_buf: *mut *mut u8, out_len: *mut usize) -> c_int {
    let boxed: Box<[u8]> = svg.into_bytes().into_boxed_slice();
    let len = boxed.len();
    let ptr = Box::into_raw(boxed) as *mut u8;
    unsafe {
        *out_buf = ptr;
        *out_len = len;
    }
    MDV_DIAGRAM_OK
}

/// Shared prologue: validate the out-params, preset them to (null, 0) so
/// even an early error leaves a state the host can rely on, and decode the
/// input. Returns the source on success, or the error code to hand back.
///
/// # Safety
/// Same contract as the public entry points below.
unsafe fn prepare<'a>(
    input: *const u8,
    input_len: usize,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> Result<&'a str, c_int> {
    if out_buf.is_null() || out_len.is_null() {
        return Err(MDV_DIAGRAM_ERR_NULL_INPUT);
    }
    *out_buf = std::ptr::null_mut();
    *out_len = 0;
    input_to_str(input, input_len).ok_or(MDV_DIAGRAM_ERR_NULL_INPUT)
}

/// Renders Mermaid source to SVG.
///
/// # Safety
/// `input` must be valid for `input_len` bytes; `out_buf`/`out_len` must be
/// valid writable pointers. On `MDV_DIAGRAM_OK` the caller owns `*out_buf`
/// and must release it with `markdownview_diagrams_free`.
#[no_mangle]
pub unsafe extern "C" fn markdownview_diagrams_render_mermaid(
    input: *const u8,
    input_len: usize,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> c_int {
    let source = match prepare(input, input_len, out_buf, out_len) {
        Ok(s) => s,
        Err(code) => return code,
    };
    // catch_unwind is load-bearing, not belt-and-braces: this crate builds
    // with panic = "unwind" precisely so a panic here becomes an error code
    // instead of aborting Double Commander. See Cargo.toml.
    match catch_unwind(AssertUnwindSafe(|| mermaid_little::convert(source))) {
        Ok(Ok(svg)) => deliver(svg, out_buf, out_len),
        Ok(Err(_)) => MDV_DIAGRAM_ERR_RENDER,
        Err(_) => MDV_DIAGRAM_ERR_RENDER,
    }
}

/// Renders PlantUML source to SVG.
///
/// # Safety
/// As `markdownview_diagrams_render_mermaid`.
#[no_mangle]
pub unsafe extern "C" fn markdownview_diagrams_render_plantuml(
    input: *const u8,
    input_len: usize,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> c_int {
    let source = match prepare(input, input_len, out_buf, out_len) {
        Ok(s) => s,
        Err(code) => return code,
    };
    match catch_unwind(AssertUnwindSafe(|| plantuml_little::convert(source))) {
        Ok(Ok(svg)) => deliver(svg, out_buf, out_len),
        Ok(Err(_)) => MDV_DIAGRAM_ERR_RENDER,
        Err(_) => MDV_DIAGRAM_ERR_RENDER,
    }
}

/// Releases a buffer handed out by either render function.
///
/// # Safety
/// `buf`/`len` must be exactly what a render call returned, and must not
/// have been freed already.
#[no_mangle]
pub unsafe extern "C" fn markdownview_diagrams_free(buf: *mut u8, len: usize) {
    if buf.is_null() {
        return;
    }
    drop(Box::from_raw(std::slice::from_raw_parts_mut(buf, len) as *mut [u8]));
}

/// The text-measurement bridge both renderers require.
///
/// Built with `metrics-ffi-callback`, neither crate measures text itself:
/// node widths, lifeline spacing and label boxes are all computed from
/// what this returns, so it MUST be installed before the first render.
/// One registration covers both, since they share `supramark-font-metrics`.
///
/// # Safety
/// `cb` must remain callable for the process's lifetime.
#[no_mangle]
pub unsafe extern "C" fn markdownview_diagrams_install_metrics_callback(
    cb: font_metrics::ffi_callback::MeasureTextFfi,
) {
    font_metrics::ffi_callback::install_ffi_metrics_callback(cb);
}
