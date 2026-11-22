/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The windows that remote layers are shown in, which the presentation takes as children of its own windows. They live
//! on a thread of their own that does nothing but pump their messages, so that the presentation, which waits for its
//! child windows while it moves or resizes its own, never waits for the main thread of this process.

use std::sync::OnceLock;
use windows::Win32::Foundation::{GetLastError, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext};
use windows::Win32::UI::WindowsAndMessaging::{
    CS_OWNDC, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GetMessageW, MSG, RegisterClassExW,
    SendMessageW, TranslateMessage, WM_APP, WM_CLOSE, WM_ERASEBKGND, WNDCLASSEXW, WS_CHILDWINDOW, WS_DISABLED,
    WS_EX_NOPARENTNOTIFY, WS_EX_NOREDIRECTIONBITMAP, WS_EX_TOOLWINDOW, WS_POPUP, WS_VISIBLE,
};
use windows::core::{PCWSTR, w};

const CREATE_CHILD_WINDOW_MESSAGE: u32 = WM_APP + 1;
const DESTROY_CHILD_WINDOW_MESSAGE: u32 = WM_APP + 2;
const PARKING_WINDOW_CLASS_NAME: PCWSTR = w!("LadybirdCompositorParkingWindow");
const CHILD_WINDOW_CLASS_NAME: PCWSTR = w!("LadybirdCompositorWindow");

/// The window that the child windows belong to until the presentation takes them, which their thread is reached through.
fn parking_window() -> Option<HWND> {
    // The handle is kept as an address, which is the same on every thread.
    static PARKING_WINDOW: OnceLock<Option<usize>> = OnceLock::new();
    let parking_window = *PARKING_WINDOW.get_or_init(|| {
        let (sender, receiver) = std::sync::mpsc::channel();
        let thread = std::thread::Builder::new()
            .name("RemoteLayerWindows".into())
            .spawn(move || run(&sender));
        let parking_window = thread.ok().and_then(|_| receiver.recv().ok().flatten());
        if parking_window.is_none() {
            eprintln!("Compositor: Unable to start the thread of the windows of remote layers");
        }
        parking_window
    });
    parking_window.map(|address| HWND(address as *mut _))
}

fn run(sender: &std::sync::mpsc::Sender<Option<usize>>) {
    // SAFETY: The calls take values this owns, and the classes and the window live for as long as the process.
    unsafe {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        let instance = GetModuleHandleW(None).unwrap_or_default();
        let parking_window_class = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            lpfnWndProc: Some(parking_window_procedure),
            hInstance: instance.into(),
            lpszClassName: PARKING_WINDOW_CLASS_NAME,
            ..Default::default()
        };
        let child_window_class = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_OWNDC,
            lpfnWndProc: Some(child_window_procedure),
            hInstance: instance.into(),
            lpszClassName: CHILD_WINDOW_CLASS_NAME,
            ..Default::default()
        };
        let mut parking_window = None;
        if RegisterClassExW(&raw const parking_window_class) != 0
            && RegisterClassExW(&raw const child_window_class) != 0
        {
            parking_window = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                PARKING_WINDOW_CLASS_NAME,
                w!(""),
                WS_POPUP | WS_DISABLED,
                0,
                0,
                1,
                1,
                None,
                None,
                Some(instance.into()),
                None,
            )
            .ok();
        }
        let _ = sender.send(parking_window.map(|window| window.0 as usize));
        if parking_window.is_none() {
            return;
        }
        let mut message = MSG::default();
        while GetMessageW(&raw mut message, None, 0, 0).0 > 0 {
            let _ = TranslateMessage(&raw const message);
            DispatchMessageW(&raw const message);
        }
    }
}

extern "system" fn parking_window_procedure(window: HWND, message: u32, w_param: WPARAM, l_param: LPARAM) -> LRESULT {
    // SAFETY: The window procedure runs on the thread of the window, with the arguments of its message.
    unsafe {
        match message {
            CREATE_CHILD_WINDOW_MESSAGE => {
                let child_window = CreateWindowExW(
                    WS_EX_NOPARENTNOTIFY | WS_EX_NOREDIRECTIONBITMAP,
                    CHILD_WINDOW_CLASS_NAME,
                    w!(""),
                    WS_CHILDWINDOW | WS_DISABLED | WS_VISIBLE,
                    0,
                    0,
                    1,
                    1,
                    Some(window),
                    None,
                    GetModuleHandleW(None).ok().map(Into::into),
                    None,
                );
                match child_window {
                    Ok(child_window) => LRESULT(child_window.0 as isize),
                    Err(_) => {
                        eprintln!(
                            "Compositor: Unable to create the window of a remote layer: error {}",
                            GetLastError().0
                        );
                        LRESULT(0)
                    }
                }
            }
            DESTROY_CHILD_WINDOW_MESSAGE => {
                let _ = DestroyWindow(HWND(w_param.0 as *mut _));
                LRESULT(0)
            }
            WM_CLOSE => LRESULT(0),
            _ => DefWindowProcW(window, message, w_param, l_param),
        }
    }
}

extern "system" fn child_window_procedure(window: HWND, message: u32, w_param: WPARAM, l_param: LPARAM) -> LRESULT {
    match message {
        WM_CLOSE => LRESULT(0),
        WM_ERASEBKGND => LRESULT(1),
        // SAFETY: The window procedure runs on the thread of the window, with the arguments of its message.
        _ => unsafe { DefWindowProcW(window, message, w_param, l_param) },
    }
}

/// A new child window for a remote layer, made on the thread of the windows.
pub(super) fn create_child_window() -> Option<HWND> {
    let parking_window = parking_window()?;
    // SAFETY: The parking window lives for as long as the process, and answers the message with a window or null.
    let result = unsafe { SendMessageW(parking_window, CREATE_CHILD_WINDOW_MESSAGE, None, None) };
    (result.0 != 0).then_some(HWND(result.0 as *mut _))
}

pub(super) fn destroy_child_window(window: HWND) {
    let Some(parking_window) = parking_window() else {
        return;
    };
    // SAFETY: The parking window lives for as long as the process, and destroys the window on its thread.
    unsafe {
        SendMessageW(
            parking_window,
            DESTROY_CHILD_WINDOW_MESSAGE,
            Some(WPARAM(window.0 as usize)),
            None,
        )
    };
}
