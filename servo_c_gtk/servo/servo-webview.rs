//! C ABI wrapper around Servo's [`servo::WebView`].
//!
//! This exposes a small, in-process C API so a GTK widget (or any other C
//! host) can create a Servo webview, drive its event loop, feed it pointer
//! input and receive rendered frames as tightly-packed RGBA8 buffers.
//!
//! # Threading
//!
//! Servo's `Servo`, `WebView` and `RenderingContext` are `Rc`-based and are
//! **not** `Send`/`Sync`. Every function in this module must be called from the
//! same thread that created the handle (the GTK main thread). Calling any of
//! them from another thread is undefined behaviour.
//!
//! # Lifecycle from the C side
//!
//! 1. `servo_webview_new()` — create a handle.
//! 2. `servo_webview_set_frame_ready_callback()` — register a frame sink.
//! 3. `servo_webview_load_uri()` / input / `servo_webview_resize()` as needed.
//! 4. `servo_webview_spin()` — call repeatedly from a GTK tick/timeout source;
//!    the frame-ready callback fires synchronously from inside this call.
//! 5. `servo_webview_free()` — destroy the handle.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::path::PathBuf;
use std::ptr;
use std::rc::Rc;
use std::sync::Once;

use euclid::{Point2D, Scale};
use servo::{
    Code, Cursor, DeviceIntRect, DeviceVector2D, InputEvent, JSValue, JavaScriptEvaluationError,
    AuthenticationRequest, ContextMenu, ContextMenuAction, ContextMenuItem,
    CreateNewWebViewRequest, EmbedderControl, EmbedderControlId, FilePicker, Key, KeyState,
    KeyboardEvent, LoadStatus, Location,
    Modifiers, MouseButton, MouseButtonAction, MouseButtonEvent, MouseMoveEvent, NamedKey,
    PermissionFeature, PermissionRequest, PrefValue, Preferences, RenderingContext, Scroll,
    Servo, ServoBuilder, SimpleDialog,
    SoftwareRenderingContext, WebView, WebViewBuilder, WebViewDelegate, WebViewPoint,
    WebViewVector,
};
use url::Url;

/// Called once per rendered frame with a tightly-packed RGBA8 buffer.
///
/// The pointer is only valid for the duration of the call; the host **must**
/// copy the pixels before returning. `width`/`height` are in device pixels and
/// the stride is always `width * 4` bytes.
pub type ServoFrameReadyCallback =
    extern "C" fn(rgba: *const u8, width: u32, height: u32, user_data: *mut c_void);

/// Called when the page requests a different cursor. `name` is a
/// NUL-terminated CSS cursor name (e.g. `"pointer"`), valid only for the
/// duration of the call.
pub type ServoCursorChangedCallback =
    extern "C" fn(name: *const c_char, user_data: *mut c_void);

/// Called when the webview's URL changes — navigation, redirect, link
/// activation or history traversal. `url` is a NUL-terminated UTF-8 string,
/// valid only for the duration of the call; copy it before returning.
pub type ServoUrlChangedCallback =
    extern "C" fn(url: *const c_char, user_data: *mut c_void);

/// Called when the page title changes. `title` is a NUL-terminated UTF-8
/// string valid only for the duration of the call, or NULL when the page has
/// no title (a fresh navigation clears it).
pub type ServoTitleChangedCallback =
    extern "C" fn(title: *const c_char, user_data: *mut c_void);

/// Called when the load progresses. `status` is a `servo_load_status` value
/// mirroring the `SERVO_LOAD_STATUS_*` constants in `servo-webview.h`.
pub type ServoLoadStatusChangedCallback = extern "C" fn(status: u32, user_data: *mut c_void);

/// Called when the session history changes, with the new availability of the
/// back and forward entries. Fires on navigation and on history traversal.
pub type ServoHistoryChangedCallback =
    extern "C" fn(can_go_back: bool, can_go_forward: bool, user_data: *mut c_void);

/// Called when web content opens a [simple dialog][spec] — `alert()`,
/// `confirm()` or `prompt()`.
///
/// The dialog is *not* answered when this returns: the host shows its own UI
/// and later calls [`servo_webview_dialog_respond`] with `request_id`. Until
/// then the page's script is blocked. `message` is content-controlled text,
/// valid only for the duration of the call; `default_value` is the prompt's
/// initial text and is NULL for alerts and confirms.
///
/// [spec]: https://html.spec.whatwg.org/multipage/#simple-dialogs
pub type ServoDialogCallback = extern "C" fn(
    request_id: u64,
    dialog_type: u32,
    message: *const c_char,
    default_value: *const c_char,
    user_data: *mut c_void,
);

/// Called when web content activates an `<input type=file>`.
///
/// The picker is *not* answered when this returns: the host shows its own file
/// chooser and later calls [`servo_webview_file_picker_respond`] with
/// `request_id`. `filter_patterns` is an array of `filter_pattern_count` bare
/// filename extensions with no leading dot (e.g. `"png"`), valid only for the
/// duration of the call; an empty array means any file is acceptable.
pub type ServoFilePickerCallback = extern "C" fn(
    request_id: u64,
    filter_patterns: *const *const c_char,
    filter_pattern_count: usize,
    allow_multiple: bool,
    user_data: *mut c_void,
);

/// Actions a context-menu item can carry. Mirrors the
/// `SERVO_CONTEXT_MENU_ACTION_*` constants in `servo-webview.h` — keep the two
/// in sync.
mod servo_context_menu_action {
    pub const GO_BACK: u32 = 0;
    pub const GO_FORWARD: u32 = 1;
    pub const RELOAD: u32 = 2;
    pub const COPY_LINK: u32 = 3;
    pub const OPEN_LINK_IN_NEW_WEBVIEW: u32 = 4;
    pub const COPY_IMAGE_LINK: u32 = 5;
    pub const OPEN_IMAGE_IN_NEW_VIEW: u32 = 6;
    pub const CUT: u32 = 7;
    pub const COPY: u32 = 8;
    pub const PASTE: u32 = 9;
    pub const SELECT_ALL: u32 = 10;
}

/// Map a Servo [`ContextMenuAction`] to its ABI value. The match is exhaustive
/// on purpose: an action added to a later Servo breaks this build rather than
/// silently arriving at the host as a different one.
fn context_menu_action_to_abi(action: ContextMenuAction) -> u32 {
    match action {
        ContextMenuAction::GoBack => servo_context_menu_action::GO_BACK,
        ContextMenuAction::GoForward => servo_context_menu_action::GO_FORWARD,
        ContextMenuAction::Reload => servo_context_menu_action::RELOAD,
        ContextMenuAction::CopyLink => servo_context_menu_action::COPY_LINK,
        ContextMenuAction::OpenLinkInNewWebView => {
            servo_context_menu_action::OPEN_LINK_IN_NEW_WEBVIEW
        },
        ContextMenuAction::CopyImageLink => servo_context_menu_action::COPY_IMAGE_LINK,
        ContextMenuAction::OpenImageInNewView => {
            servo_context_menu_action::OPEN_IMAGE_IN_NEW_VIEW
        },
        ContextMenuAction::Cut => servo_context_menu_action::CUT,
        ContextMenuAction::Copy => servo_context_menu_action::COPY,
        ContextMenuAction::Paste => servo_context_menu_action::PASTE,
        ContextMenuAction::SelectAll => servo_context_menu_action::SELECT_ALL,
    }
}

/// One entry of a context menu, as handed to a [`ServoContextMenuCallback`].
/// Mirrors `ServoContextMenuItem` in `servo-webview.h`.
#[repr(C)]
pub struct ServoContextMenuItem {
    /// The item's text, or NULL when this entry is a separator.
    pub label: *const c_char,
    /// A `SERVO_CONTEXT_MENU_ACTION_*` value; meaningless for a separator.
    pub action: u32,
    /// Whether the item can be chosen.
    pub enabled: bool,
}

/// Called when the user asks for a context menu on web content.
///
/// The menu is *not* answered when this returns: the host shows its own menu
/// and later calls [`servo_webview_context_menu_respond`] with `request_id` and
/// the index of the chosen item. `items` holds `item_count` entries valid only
/// for the duration of the call. `x` and `y` are the top-left of the element the
/// menu was opened on, in device pixels.
pub type ServoContextMenuCallback = extern "C" fn(
    request_id: u64,
    items: *const ServoContextMenuItem,
    item_count: usize,
    x: i32,
    y: i32,
    user_data: *mut c_void,
);

/// Called when web content asks to open a new webview — `window.open()`, or a
/// link with `target="_blank"`.
///
/// Accept it by calling [`servo_webview_create_popup`] with `request_id` from
/// inside this callback. Returning without accepting refuses the popup, which
/// is also what happens with no callback registered.
pub type ServoCreateWebViewCallback =
    extern "C" fn(request_id: u64, user_data: *mut c_void);

/// Called when a webview closes itself — `window.close()`, or the page that
/// opened a popup closing it. The host should take down whatever window is
/// showing this webview and free its handle.
pub type ServoClosedCallback = extern "C" fn(user_data: *mut c_void);

/// Called when a server or proxy asks for HTTP authentication.
///
/// The request is *not* answered when this returns: the host prompts for
/// credentials and later calls [`servo_webview_authentication_respond`] with
/// `request_id`. `url` is the URL that triggered the challenge, valid only for
/// the duration of the call. `for_proxy` distinguishes a proxy challenge from
/// an origin one — the two must not be confused, since credentials for one are
/// not credentials for the other.
pub type ServoAuthenticationCallback = extern "C" fn(
    request_id: u64,
    url: *const c_char,
    for_proxy: bool,
    user_data: *mut c_void,
);

/// Called when a page asks for a permission-gated capability.
///
/// The request is *not* answered when this returns; call
/// [`servo_webview_permission_respond`] with `request_id`. `feature` is a
/// `servo_permission` value. Never answering denies the request, which is also
/// what happens with no callback registered.
pub type ServoPermissionCallback =
    extern "C" fn(request_id: u64, feature: u32, user_data: *mut c_void);

/// Permission-gated capabilities. Mirrors the `SERVO_PERMISSION_*` constants in
/// `servo-webview.h` — keep the two in sync.
mod servo_permission {
    pub const GEOLOCATION: u32 = 0;
    pub const NOTIFICATIONS: u32 = 1;
    pub const PUSH: u32 = 2;
    pub const MIDI: u32 = 3;
    pub const CAMERA: u32 = 4;
    pub const MICROPHONE: u32 = 5;
    pub const SPEAKER: u32 = 6;
    pub const DEVICE_INFO: u32 = 7;
    pub const BACKGROUND_SYNC: u32 = 8;
    pub const BLUETOOTH: u32 = 9;
    pub const PERSISTENT_STORAGE: u32 = 10;
    pub const SCREEN_WAKE_LOCK: u32 = 11;
}

/// Map a Servo [`PermissionFeature`] to its `servo_permission` ABI value. The
/// match is exhaustive on purpose: a feature added to a later Servo breaks this
/// build rather than silently arriving at the host as something else.
fn permission_feature_to_abi(feature: PermissionFeature) -> u32 {
    match feature {
        PermissionFeature::Geolocation => servo_permission::GEOLOCATION,
        PermissionFeature::Notifications => servo_permission::NOTIFICATIONS,
        PermissionFeature::Push => servo_permission::PUSH,
        PermissionFeature::Midi => servo_permission::MIDI,
        PermissionFeature::Camera => servo_permission::CAMERA,
        PermissionFeature::Microphone => servo_permission::MICROPHONE,
        PermissionFeature::Speaker => servo_permission::SPEAKER,
        PermissionFeature::DeviceInfo => servo_permission::DEVICE_INFO,
        PermissionFeature::BackgroundSync => servo_permission::BACKGROUND_SYNC,
        PermissionFeature::Bluetooth => servo_permission::BLUETOOTH,
        PermissionFeature::PersistentStorage => servo_permission::PERSISTENT_STORAGE,
        PermissionFeature::ScreenWakeLock => servo_permission::SCREEN_WAKE_LOCK,
    }
}

/// Called when Servo withdraws a request the host has not answered yet — the
/// page navigated away, or the element went out of the document. The host
/// should take down whatever UI it put up for `request_id`; responding to it
/// afterwards is harmless and does nothing.
pub type ServoRequestCancelledCallback =
    extern "C" fn(request_id: u64, user_data: *mut c_void);

/// Dialog kinds passed to a [`ServoDialogCallback`]. Mirrors the
/// `SERVO_DIALOG_*` constants in `servo-webview.h` — keep the two in sync.
mod servo_dialog {
    pub const ALERT: u32 = 0;
    pub const CONFIRM: u32 = 1;
    pub const PROMPT: u32 = 2;
}

/// A request Servo has handed to the embedder and is waiting on an answer for.
///
/// Each variant owns the Servo-side request object. Dropping one without an
/// explicit answer is safe: every one of these types answers with its
/// conservative default from `Drop` (dismiss/cancel), so tearing the handle
/// down while dialogs are open never leaves script blocked forever.
enum PendingRequest {
    SimpleDialog(SimpleDialog),
    FilePicker(FilePicker),
    Authentication(AuthenticationRequest),
    Permission(PermissionRequest),
    ContextMenu(ContextMenu),
    CreateWebView(CreateNewWebViewRequest),
}

/// A [`PendingRequest`] plus the engine-side id it arrived with, which is what
/// `hide_embedder_control` names when Servo withdraws it. Requests that do not
/// come from `show_embedder_control` — authentication and permission prompts —
/// cannot be withdrawn and carry `None`.
struct Pending {
    control_id: Option<EmbedderControlId>,
    request: PendingRequest,
}

/// The requests currently awaiting an answer from the host, keyed by the id
/// handed out to C.
///
/// `EmbedderControlId` is only `PartialEq`, not `Hash`, so withdrawal scans for
/// a matching id rather than looking it up. The map holds at most a couple of
/// entries in practice — script blocks on a simple dialog, so a page cannot
/// stack them up.
#[derive(Default)]
struct RequestRegistry {
    requests: RefCell<HashMap<u64, Pending>>,
    next_id: Cell<u64>,
}

impl RequestRegistry {
    /// Store `request` and return the id C should use to answer it.
    fn insert(&self, control_id: Option<EmbedderControlId>, request: PendingRequest) -> u64 {
        let id = self.next_id.get().wrapping_add(1);
        self.next_id.set(id);
        self.requests.borrow_mut().insert(
            id,
            Pending {
                control_id,
                request,
            },
        );
        id
    }

    /// Remove and return a request, or `None` if it was already answered or
    /// withdrawn (a host answering twice, or answering a stale id).
    fn take(&self, id: u64) -> Option<PendingRequest> {
        self.requests
            .borrow_mut()
            .remove(&id)
            .map(|pending| pending.request)
    }

    /// Remove the request carrying `control_id`, returning the id C knows it
    /// by so the withdrawal can be reported.
    fn take_by_control_id(&self, control_id: EmbedderControlId) -> Option<u64> {
        let mut requests = self.requests.borrow_mut();
        let id = requests
            .iter()
            .find(|(_, pending)| pending.control_id == Some(control_id))
            .map(|(id, _)| *id)?;
        requests.remove(&id);
        Some(id)
    }
}

struct FrameCallback {
    func: ServoFrameReadyCallback,
    user_data: *mut c_void,
}

struct CursorCallback {
    func: ServoCursorChangedCallback,
    user_data: *mut c_void,
}

struct UrlCallback {
    func: ServoUrlChangedCallback,
    user_data: *mut c_void,
}

struct TitleCallback {
    func: ServoTitleChangedCallback,
    user_data: *mut c_void,
}

struct LoadStatusCallback {
    func: ServoLoadStatusChangedCallback,
    user_data: *mut c_void,
}

struct HistoryCallback {
    func: ServoHistoryChangedCallback,
    user_data: *mut c_void,
}

struct DialogCallback {
    func: ServoDialogCallback,
    user_data: *mut c_void,
}

struct RequestCancelledCallback {
    func: ServoRequestCancelledCallback,
    user_data: *mut c_void,
}

struct FilePickerCallback {
    func: ServoFilePickerCallback,
    user_data: *mut c_void,
}

struct AuthenticationCallback {
    func: ServoAuthenticationCallback,
    user_data: *mut c_void,
}

struct PermissionCallback {
    func: ServoPermissionCallback,
    user_data: *mut c_void,
}

struct ContextMenuCallback {
    func: ServoContextMenuCallback,
    user_data: *mut c_void,
}

struct CreateWebViewCallback {
    func: ServoCreateWebViewCallback,
    user_data: *mut c_void,
}

struct ClosedCallback {
    func: ServoClosedCallback,
    user_data: *mut c_void,
}

/// Servo delegate that turns presented frames, cursor changes and URL changes
/// into calls into the registered C callbacks.
struct EmbedderDelegate {
    rendering_context: Rc<dyn RenderingContext>,
    frame_callback: RefCell<Option<FrameCallback>>,
    cursor_callback: RefCell<Option<CursorCallback>>,
    url_callback: RefCell<Option<UrlCallback>>,
    title_callback: RefCell<Option<TitleCallback>>,
    load_status_callback: RefCell<Option<LoadStatusCallback>>,
    history_callback: RefCell<Option<HistoryCallback>>,
    dialog_callback: RefCell<Option<DialogCallback>>,
    file_picker_callback: RefCell<Option<FilePickerCallback>>,
    authentication_callback: RefCell<Option<AuthenticationCallback>>,
    permission_callback: RefCell<Option<PermissionCallback>>,
    context_menu_callback: RefCell<Option<ContextMenuCallback>>,
    create_webview_callback: RefCell<Option<CreateWebViewCallback>>,
    closed_callback: RefCell<Option<ClosedCallback>>,
    request_cancelled_callback: RefCell<Option<RequestCancelledCallback>>,
    requests: RequestRegistry,
}

impl EmbedderDelegate {
    fn new(rendering_context: Rc<dyn RenderingContext>) -> Self {
        Self {
            rendering_context,
            frame_callback: RefCell::new(None),
            cursor_callback: RefCell::new(None),
            url_callback: RefCell::new(None),
            title_callback: RefCell::new(None),
            load_status_callback: RefCell::new(None),
            history_callback: RefCell::new(None),
            dialog_callback: RefCell::new(None),
            file_picker_callback: RefCell::new(None),
            authentication_callback: RefCell::new(None),
            permission_callback: RefCell::new(None),
            context_menu_callback: RefCell::new(None),
            create_webview_callback: RefCell::new(None),
            closed_callback: RefCell::new(None),
            request_cancelled_callback: RefCell::new(None),
            requests: RequestRegistry::default(),
        }
    }
}

impl WebViewDelegate for EmbedderDelegate {
    fn notify_new_frame_ready(&self, webview: WebView) {
        // Paint the frame, read it back, then present. `read_to_image` reads the
        // *back* buffer, which is what `paint()` just wrote; `present()` swaps it
        // out (with `PreserveBuffer::No`), so reading must happen *before*
        // presenting or we capture the stale, swapped-in buffer instead.
        let size = self.rendering_context.size2d().to_i32();
        let viewport_rect = DeviceIntRect::from_origin_and_size(Point2D::origin(), size);

        webview.paint();

        let image = self.rendering_context.read_to_image(viewport_rect);
        self.rendering_context.present();

        let Some(image) = image else {
            return;
        };

        let width = image.width();
        let height = image.height();
        let data = image.into_raw();

        // Copy the callback out before invoking it so the C side may safely
        // re-register a callback from within the call (no RefCell held).
        let cb = self
            .frame_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        if let Some((func, user_data)) = cb {
            func(data.as_ptr(), width, height, user_data);
        }
    }

    fn notify_cursor_changed(&self, _webview: WebView, cursor: Cursor) {
        let Some(cb) = self.cursor_callback.borrow().as_ref().map(|c| (c.func, c.user_data))
        else {
            return;
        };
        if let Ok(name) = CString::new(cursor_css_name(cursor)) {
            (cb.0)(name.as_ptr(), cb.1);
        }
    }

    fn notify_url_changed(&self, _webview: WebView, url: Url) {
        let Some(cb) = self.url_callback.borrow().as_ref().map(|c| (c.func, c.user_data))
        else {
            return;
        };
        if let Ok(url) = CString::new(url.as_str()) {
            (cb.0)(url.as_ptr(), cb.1);
        }
    }

    fn notify_page_title_changed(&self, _webview: WebView, title: Option<String>) {
        let Some(cb) = self.title_callback.borrow().as_ref().map(|c| (c.func, c.user_data))
        else {
            return;
        };
        // A title with an interior NUL cannot cross the C boundary; report it
        // the same way as no title at all rather than dropping the event.
        match title.and_then(|title| CString::new(title).ok()) {
            Some(title) => (cb.0)(title.as_ptr(), cb.1),
            None => (cb.0)(ptr::null(), cb.1),
        }
    }

    fn notify_load_status_changed(&self, _webview: WebView, status: LoadStatus) {
        let Some(cb) = self
            .load_status_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data))
        else {
            return;
        };
        (cb.0)(load_status_to_abi(status), cb.1);
    }

    fn notify_history_changed(&self, webview: WebView, _entries: Vec<Url>, _current: usize) {
        let Some(cb) = self.history_callback.borrow().as_ref().map(|c| (c.func, c.user_data))
        else {
            return;
        };
        // Servo updates the webview's back/forward list before invoking the
        // delegate, so querying it here reports the post-change state.
        (cb.0)(webview.can_go_back(), webview.can_go_forward(), cb.1);
    }

    fn show_embedder_control(&self, _webview: WebView, embedder_control: EmbedderControl) {
        let control_id = embedder_control.id();

        match embedder_control {
            EmbedderControl::SimpleDialog(dialog) => self.show_simple_dialog(control_id, dialog),
            EmbedderControl::FilePicker(picker) => self.show_file_picker(control_id, picker),
            EmbedderControl::ContextMenu(menu) => self.show_context_menu(control_id, menu),
            // Select-element pickers, colour pickers and IME are not wired up
            // yet. Dropping the control answers it with its default (dismissed
            // / no selection) rather than leaving script waiting.
            _ => {},
        }
    }

    fn request_authentication(
        &self,
        _webview: WebView,
        authentication_request: AuthenticationRequest,
    ) {
        let cb = self
            .authentication_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        // With no callback the request is dropped, which answers it with no
        // credentials — the load then fails as unauthenticated rather than
        // proceeding, which is the fail-closed outcome.
        let Some((func, user_data)) = cb else {
            return;
        };

        let Ok(url) = CString::new(authentication_request.url().as_str()) else {
            return;
        };
        let for_proxy = authentication_request.for_proxy();

        let id = self.requests.insert(
            None,
            PendingRequest::Authentication(authentication_request),
        );

        func(id, url.as_ptr(), for_proxy, user_data);
    }

    fn request_permission(&self, _webview: WebView, permission_request: PermissionRequest) {
        let cb = self
            .permission_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        // With no callback the request is dropped, which denies it: Servo
        // builds these with AllowOrDeny::Deny as the default response.
        let Some((func, user_data)) = cb else {
            return;
        };

        let feature = permission_feature_to_abi(permission_request.feature());
        let id = self
            .requests
            .insert(None, PendingRequest::Permission(permission_request));

        func(id, feature, user_data);
    }

    fn request_create_new(&self, _parent_webview: WebView, request: CreateNewWebViewRequest) {
        let cb = self
            .create_webview_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        // With no callback the request is dropped, which refuses the popup.
        let Some((func, user_data)) = cb else {
            return;
        };

        let id = self
            .requests
            .insert(None, PendingRequest::CreateWebView(request));

        func(id, user_data);

        // The host is expected to accept from inside the callback. Anything
        // still here afterwards is dropped, which refuses the popup rather than
        // leaving window.open() waiting on an answer that will not come.
        drop(self.requests.take(id));
    }

    fn notify_closed(&self, _webview: WebView) {
        let cb = self
            .closed_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        if let Some((func, user_data)) = cb {
            func(user_data);
        }
    }

    fn hide_embedder_control(&self, _webview: WebView, control_id: EmbedderControlId) {
        // Dropping the stored request answers it with its conservative default.
        let Some(id) = self.requests.take_by_control_id(control_id) else {
            return;
        };

        let cb = self
            .request_cancelled_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        if let Some((func, user_data)) = cb {
            func(id, user_data);
        }
    }
}

impl EmbedderDelegate {
    /// Hand a script-initiated dialog to the host and keep it alive until the
    /// host answers. With no dialog callback registered the dialog is dropped
    /// here, which answers it (alert: OK, confirm/prompt: cancel) rather than
    /// blocking the page's script forever.
    fn show_simple_dialog(&self, control_id: EmbedderControlId, dialog: SimpleDialog) {
        let cb = self
            .dialog_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        let Some((func, user_data)) = cb else {
            return;
        };

        let dialog_type = match dialog {
            SimpleDialog::Alert(_) => servo_dialog::ALERT,
            SimpleDialog::Confirm(_) => servo_dialog::CONFIRM,
            SimpleDialog::Prompt(_) => servo_dialog::PROMPT,
        };

        // Both strings are content-controlled, so an interior NUL is possible;
        // fall back to an empty string rather than dropping the dialog.
        let message = CString::new(dialog.message()).unwrap_or_default();
        let default_value = match &dialog {
            SimpleDialog::Prompt(prompt) => {
                Some(CString::new(prompt.current_value()).unwrap_or_default())
            },
            _ => None,
        };

        let id = self
            .requests
            .insert(Some(control_id), PendingRequest::SimpleDialog(dialog));

        func(
            id,
            dialog_type,
            message.as_ptr(),
            default_value
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
            user_data,
        );
    }

    /// Hand an `<input type=file>` activation to the host and keep the picker
    /// alive until the host answers. With no callback registered the picker is
    /// dropped here, which answers it as dismissed.
    fn show_file_picker(&self, control_id: EmbedderControlId, picker: FilePicker) {
        let cb = self
            .file_picker_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        let Some((func, user_data)) = cb else {
            return;
        };

        // Keep the CStrings alive for the duration of the call; the array we
        // pass out only borrows their pointers.
        let patterns: Vec<CString> = picker
            .filter_patterns()
            .iter()
            .filter_map(|pattern| CString::new(pattern.0.as_str()).ok())
            .collect();
        let pattern_ptrs: Vec<*const c_char> =
            patterns.iter().map(|pattern| pattern.as_ptr()).collect();
        let allow_multiple = picker.allow_select_multiple();

        let id = self
            .requests
            .insert(Some(control_id), PendingRequest::FilePicker(picker));

        func(
            id,
            pattern_ptrs.as_ptr(),
            pattern_ptrs.len(),
            allow_multiple,
            user_data,
        );
    }

    /// Hand a context menu to the host and keep it alive until the host answers.
    /// With no callback registered the menu is dropped here, which dismisses it.
    fn show_context_menu(&self, control_id: EmbedderControlId, menu: ContextMenu) {
        let cb = self
            .context_menu_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        let Some((func, user_data)) = cb else {
            return;
        };

        // Keep the labels alive for the duration of the call; the item array
        // only borrows their pointers.
        let labels: Vec<Option<CString>> = menu
            .items()
            .iter()
            .map(|item| match item {
                // A label with an interior NUL becomes empty rather than None:
                // None is what marks a separator in the array below.
                ContextMenuItem::Item { label, .. } => {
                    Some(CString::new(label.as_str()).unwrap_or_default())
                },
                ContextMenuItem::Separator => None,
            })
            .collect();

        let items: Vec<ServoContextMenuItem> = menu
            .items()
            .iter()
            .zip(labels.iter())
            .map(|(item, label)| match item {
                ContextMenuItem::Item {
                    action, enabled, ..
                } => ServoContextMenuItem {
                    label: label.as_ref().map_or(ptr::null(), |label| label.as_ptr()),
                    action: context_menu_action_to_abi(*action),
                    enabled: *enabled,
                },
                ContextMenuItem::Separator => ServoContextMenuItem {
                    label: ptr::null(),
                    action: 0,
                    enabled: false,
                },
            })
            .collect();

        let position = menu.position();
        let (x, y) = (position.min.x, position.min.y);

        let id = self
            .requests
            .insert(Some(control_id), PendingRequest::ContextMenu(menu));

        func(id, items.as_ptr(), items.len(), x, y, user_data);
    }
}

/// Load-progress values understood by the C ABI. Mirrors the
/// `SERVO_LOAD_STATUS_*` constants in `servo-webview.h` — keep the two in sync.
mod servo_load_status {
    pub const STARTED: u32 = 0;
    pub const HEAD_PARSED: u32 = 1;
    pub const COMPLETE: u32 = 2;
}

/// Map a Servo [`LoadStatus`] to its `servo_load_status` ABI value.
fn load_status_to_abi(status: LoadStatus) -> u32 {
    match status {
        LoadStatus::Started => servo_load_status::STARTED,
        LoadStatus::HeadParsed => servo_load_status::HEAD_PARSED,
        LoadStatus::Complete => servo_load_status::COMPLETE,
    }
}

/// Opaque handle handed to C. Owns the whole Servo instance for one webview.
pub struct ServoWebViewHandle {
    servo: Servo,
    webview: WebView,
    delegate: Rc<EmbedderDelegate>,
    // Kept alive for as long as the webview lives; the delegate also holds a
    // type-erased clone of the same context.
    _rendering_context: Rc<SoftwareRenderingContext>,
}

static INIT: Once = Once::new();

/// One-time, process-global initialisation of the TLS crypto provider. Safe to
/// call repeatedly; only the first call has effect.
///
/// Servo's resource reader is intentionally *not* installed here. As of `servo`
/// 0.2.0 the reader is registered at compile time via `submit_resource_reader!`,
/// and the `servo-default-resources` crate (an unconditional dependency) bakes
/// one in — so a reader is always present and the old runtime `resources::set`
/// API no longer exists. There is no supported way to override it at runtime.
fn ensure_initialized() {
    INIT.call_once(|| {
        // Servo performs HTTPS itself and expects a default rustls crypto
        // provider to be installed by the embedder. Fail closed is not an
        // option here (it would abort the process), but a duplicate install is
        // harmless, so ignore the result.
        let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();
    });
}

/// Map a Servo [`Cursor`] to its CSS cursor name.
fn cursor_css_name(cursor: Cursor) -> &'static str {
    match cursor {
        Cursor::Default => "default",
        Cursor::Pointer => "pointer",
        Cursor::Text => "text",
        Cursor::Wait => "wait",
        Cursor::Help => "help",
        Cursor::Crosshair => "crosshair",
        Cursor::Move => "move",
        Cursor::EResize => "e-resize",
        Cursor::NeResize => "ne-resize",
        Cursor::NwResize => "nw-resize",
        Cursor::NResize => "n-resize",
        Cursor::SeResize => "se-resize",
        Cursor::SwResize => "sw-resize",
        Cursor::SResize => "s-resize",
        Cursor::WResize => "w-resize",
        Cursor::EwResize => "ew-resize",
        Cursor::NsResize => "ns-resize",
        Cursor::NeswResize => "nesw-resize",
        Cursor::NwseResize => "nwse-resize",
        Cursor::ColResize => "col-resize",
        Cursor::RowResize => "row-resize",
        Cursor::AllScroll => "all-scroll",
        Cursor::ZoomIn => "zoom-in",
        Cursor::ZoomOut => "zoom-out",
        Cursor::Alias => "alias",
        Cursor::Cell => "cell",
        Cursor::Copy => "copy",
        Cursor::ContextMenu => "context-menu",
        Cursor::NoDrop => "no-drop",
        Cursor::NotAllowed => "not-allowed",
        Cursor::Grab => "grab",
        Cursor::Grabbing => "grabbing",
        Cursor::VerticalText => "vertical-text",
        Cursor::Progress => "progress",
        _ => "default",
    }
}

/// Borrow a handle pointer as a reference, returning early on NULL.
///
/// # Safety
/// `ptr` must be NULL or a pointer returned by [`servo_webview_new`] that has
/// not yet been passed to [`servo_webview_free`].
unsafe fn as_handle<'a>(ptr: *mut ServoWebViewHandle) -> Option<&'a ServoWebViewHandle> {
    unsafe { ptr.as_ref() }
}

/// Experimental Web-platform features, gated behind preferences that default to
/// off. Enabling all of these is the equivalent of servoshell's
/// `--enable-experimental-web-platform-features` flag.
///
/// This mirrors the `// feature:`-annotated preferences in `servo-config`'s
/// `prefs.rs` (v0.3.0). Servo exposes no runtime "is experimental" query, so the
/// list is maintained by hand — revisit it when bumping the `servo` dependency.
/// Names are looked up with [`Preferences::exists`] before being set, so an entry
/// that disappears in a future version is skipped rather than panicking.
const EXPERIMENTAL_WEB_PLATFORM_FEATURES: &[&str] = &[
    "dom_webgpu_enabled",
    "dom_abort_controller_enabled",
    "dom_adoptedstylesheet_enabled",
    "dom_async_clipboard_enabled",
    "dom_canvas_capture_enabled",
    "dom_cookiestore_enabled",
    "dom_credential_management_enabled",
    "dom_crypto_subtle_enabled",
    "dom_exec_command_enabled",
    "dom_fontface_enabled",
    "dom_gamepad_enabled",
    "dom_geolocation_enabled",
    "dom_wakelock_enabled",
    "dom_indexeddb_enabled",
    "dom_intersection_observer_enabled",
    "dom_mutation_observer_enabled",
    "dom_navigator_protocol_handlers_enabled",
    "dom_notification_enabled",
    "dom_offscreen_canvas_enabled",
    "dom_permissions_enabled",
    "dom_resize_observer_enabled",
    "dom_sanitizer_enabled",
    "dom_storage_manager_api_enabled",
    "dom_serviceworker_enabled",
    "dom_sharedworker_enabled",
    "dom_webgl2_enabled",
    "dom_webrtc_enabled",
    "dom_webrtc_transceiver_enabled",
    "dom_webvtt_enabled",
    "dom_webxr_layers_enabled",
    "dom_visual_viewport_enabled",
    "largest_contentful_paint_enabled",
    "layout_columns_enabled",
    "layout_grid_enabled",
    "layout_variable_fonts_enabled",
    "layout_writing_mode_enabled",
];

/// Build the default [`Preferences`] with every experimental Web-platform
/// feature in [`EXPERIMENTAL_WEB_PLATFORM_FEATURES`] turned on.
fn experimental_preferences() -> Preferences {
    let mut preferences = Preferences::default();
    for name in EXPERIMENTAL_WEB_PLATFORM_FEATURES {
        if Preferences::exists(name) {
            preferences.set_value(name, PrefValue::Bool(true));
        }
    }
    preferences
}

/// Create a new Servo webview.
///
/// * `width` / `height` — initial surface size in device pixels.
/// * `initial_uri` — the URL to load when the webview is created, or NULL to
///   start on `about:blank`. Passing the initial URL here (rather than calling
///   [`servo_webview_load_uri`] right after creation) is required for it to
///   take effect: Servo creates the top-level browsing context together with
///   this URL, whereas a separate `load` issued before the browsing context
///   exists is dropped by the constellation. Invalid URLs fall back to
///   `about:blank`.
///
/// Returns an owning handle, or NULL on failure. Free it with
/// [`servo_webview_free`].
///
/// # Safety
/// `initial_uri` must be NULL or point to a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_new(
    width: u32,
    height: u32,
    initial_uri: *const c_char,
) -> *mut ServoWebViewHandle {
    ensure_initialized();

    let size = dpi::PhysicalSize::new(width.max(1), height.max(1));
    let rendering_context = match SoftwareRenderingContext::new(size) {
        Ok(context) => Rc::new(context),
        Err(_) => return ptr::null_mut(),
    };

    let initial_url = if initial_uri.is_null() {
        None
    } else {
        unsafe { CStr::from_ptr(initial_uri) }
            .to_str()
            .ok()
            .and_then(|s| Url::parse(s).ok())
    };

    let servo = ServoBuilder::default()
        .preferences(experimental_preferences())
        .build();

    let delegate = Rc::new(EmbedderDelegate::new(rendering_context.clone()));
    let mut builder = WebViewBuilder::new(&servo, rendering_context.clone())
        .delegate(delegate.clone());
    if let Some(url) = initial_url {
        builder = builder.url(url);
    }
    let webview = builder.build();
    webview.focus();
    webview.show();

    let handle = Box::new(ServoWebViewHandle {
        servo,
        webview,
        delegate,
        _rendering_context: rendering_context,
    });
    Box::into_raw(handle)
}

/// Destroy a webview handle. Passing NULL is a no-op.
///
/// # Safety
/// `webview` must be NULL or a pointer from [`servo_webview_new`] that has not
/// already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_free(webview: *mut ServoWebViewHandle) {
    if webview.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(webview) });
}

/// Register the frame sink invoked once per presented frame. Pass a NULL
/// `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_frame_ready_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoFrameReadyCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.frame_callback.borrow_mut() =
        callback.map(|func| FrameCallback { func, user_data });
}

/// Register the cursor-change callback. Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_cursor_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoCursorChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.cursor_callback.borrow_mut() =
        callback.map(|func| CursorCallback { func, user_data });
}

/// Register the URL-change callback, invoked whenever the webview navigates to a
/// new URL. Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_url_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoUrlChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.url_callback.borrow_mut() =
        callback.map(|func| UrlCallback { func, user_data });
}

/// Register the page-title callback, invoked whenever the title changes. Pass a
/// NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_title_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoTitleChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.title_callback.borrow_mut() =
        callback.map(|func| TitleCallback { func, user_data });
}

/// Register the load-status callback, invoked as a load starts, has its `<head>`
/// parsed, and completes. Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_load_status_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoLoadStatusChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.load_status_callback.borrow_mut() =
        callback.map(|func| LoadStatusCallback { func, user_data });
}

/// Register the history-change callback, invoked whenever the session history
/// changes with the new availability of the back and forward entries. Pass a
/// NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_history_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoHistoryChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.history_callback.borrow_mut() =
        callback.map(|func| HistoryCallback { func, user_data });
}

/// Register the dialog callback, invoked when web content calls `alert()`,
/// `confirm()` or `prompt()`. Pass a NULL `callback` to clear it.
///
/// With no callback registered, dialogs are answered immediately with their
/// default (alert: OK, confirm and prompt: cancel) so that script never blocks
/// on UI that will not appear.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_dialog_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoDialogCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.dialog_callback.borrow_mut() =
        callback.map(|func| DialogCallback { func, user_data });
}

/// Register the request-withdrawn callback, invoked when Servo takes back a
/// request the host has not answered yet. Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_request_cancelled_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoRequestCancelledCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.request_cancelled_callback.borrow_mut() =
        callback.map(|func| RequestCancelledCallback { func, user_data });
}

/// Register the file-picker callback, invoked when web content activates an
/// `<input type=file>`. Pass a NULL `callback` to clear it.
///
/// With no callback registered, pickers are answered immediately as dismissed
/// so that script never blocks on UI that will not appear.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_file_picker_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoFilePickerCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.file_picker_callback.borrow_mut() =
        callback.map(|func| FilePickerCallback { func, user_data });
}

/// Register the authentication callback, invoked when a server or proxy issues
/// an HTTP authentication challenge. Pass a NULL `callback` to clear it.
///
/// With no callback registered the challenge is answered with no credentials,
/// so the load fails as unauthenticated rather than proceeding.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_authentication_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoAuthenticationCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.authentication_callback.borrow_mut() =
        callback.map(|func| AuthenticationCallback { func, user_data });
}

/// Register the permission callback, invoked when a page asks for a
/// permission-gated capability. Pass a NULL `callback` to clear it.
///
/// With no callback registered the request is denied.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_permission_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoPermissionCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.permission_callback.borrow_mut() =
        callback.map(|func| PermissionCallback { func, user_data });
}

/// Register the context-menu callback, invoked when the user asks for a context
/// menu on web content. Pass a NULL `callback` to clear it.
///
/// With no callback registered the menu is dismissed immediately.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_context_menu_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoContextMenuCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.context_menu_callback.borrow_mut() =
        callback.map(|func| ContextMenuCallback { func, user_data });
}

/// Register the popup callback, invoked when web content asks to open a new
/// webview. Pass a NULL `callback` to clear it.
///
/// With no callback registered, popups are refused.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_create_webview_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoCreateWebViewCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.create_webview_callback.borrow_mut() =
        callback.map(|func| CreateWebViewCallback { func, user_data });
}

/// Register the closed callback, invoked when the webview closes itself.
/// Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_closed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoClosedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.closed_callback.borrow_mut() =
        callback.map(|func| ClosedCallback { func, user_data });
}

/// Accept a popup request reported through a [`ServoCreateWebViewCallback`] and
/// build its webview, with a surface of `width` x `height` device pixels.
///
/// Must be called from inside that callback: the request is refused as soon as
/// the callback returns.
///
/// The popup joins the parent's engine, so both handles must be driven from the
/// same thread; spinning either one advances both. The returned handle is owned
/// by the caller and must be freed with [`servo_webview_free`], and starts with
/// no callbacks registered — register them before spinning or the first frame
/// is lost.
///
/// Returns NULL for an unknown `request_id` or if the surface could not be
/// created; the popup is refused in that case.
///
/// # Safety
/// `webview` must be the valid parent handle the callback came from.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_create_popup(
    webview: *mut ServoWebViewHandle,
    request_id: u64,
    width: u32,
    height: u32,
) -> *mut ServoWebViewHandle {
    let Some(parent) = (unsafe { as_handle(webview) }) else {
        return ptr::null_mut();
    };
    let Some(PendingRequest::CreateWebView(create_request)) =
        parent.delegate.requests.take(request_id)
    else {
        return ptr::null_mut();
    };

    let size = dpi::PhysicalSize::new(width.max(1), height.max(1));
    let rendering_context = match SoftwareRenderingContext::new(size) {
        Ok(context) => Rc::new(context),
        // `create_request` is dropped here, which refuses the popup.
        Err(_) => return ptr::null_mut(),
    };

    let servo = parent.servo.clone();
    let delegate = Rc::new(EmbedderDelegate::new(rendering_context.clone()));

    let webview = create_request
        .builder(rendering_context.clone())
        .delegate(delegate.clone())
        .build();
    webview.focus();
    webview.show();

    Box::into_raw(Box::new(ServoWebViewHandle {
        servo,
        webview,
        delegate,
        _rendering_context: rendering_context,
    }))
}

/// Answer a context menu reported through a [`ServoContextMenuCallback`].
///
/// `item_index` is an index into the `items` array that came with the menu.
/// `SERVO_CONTEXT_MENU_NO_SELECTION` — or any out-of-range index, or the index
/// of a separator — dismisses the menu with no selection.
///
/// An unknown `request_id` is ignored.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_context_menu_respond(
    webview: *mut ServoWebViewHandle,
    request_id: u64,
    item_index: usize,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let Some(PendingRequest::ContextMenu(menu)) = handle.delegate.requests.take(request_id)
    else {
        return;
    };

    // Re-read the action from the menu rather than trusting an action value
    // sent back across the boundary, so a bad index can only ever dismiss.
    let action = match menu.items().get(item_index) {
        Some(ContextMenuItem::Item { action, .. }) => *action,
        _ => {
            menu.dismiss();
            return;
        },
    };

    menu.select(action);
}

/// Answer an authentication challenge reported through a
/// [`ServoAuthenticationCallback`].
///
/// Supply both `username` and `password` to authenticate. A NULL `username` or
/// `password` declines to supply credentials, and the load fails as
/// unauthenticated. Credentials that are not valid UTF-8 are treated the same
/// way rather than being sent mangled.
///
/// An unknown `request_id` is ignored.
///
/// # Safety
/// `webview` must be a valid handle; `username` and `password` must each be
/// NULL or a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_authentication_respond(
    webview: *mut ServoWebViewHandle,
    request_id: u64,
    username: *const c_char,
    password: *const c_char,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let Some(PendingRequest::Authentication(request)) =
        handle.delegate.requests.take(request_id)
    else {
        return;
    };

    if username.is_null() || password.is_null() {
        // Dropping answers with no credentials.
        return;
    }

    let (Ok(username), Ok(password)) = (
        unsafe { CStr::from_ptr(username) }.to_str(),
        unsafe { CStr::from_ptr(password) }.to_str(),
    ) else {
        return;
    };

    request.authenticate(username.to_owned(), password.to_owned());
}

/// Answer a permission request reported through a [`ServoPermissionCallback`].
///
/// An unknown `request_id` is ignored. A request that is never answered is
/// denied when the handle is dropped.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_permission_respond(
    webview: *mut ServoWebViewHandle,
    request_id: u64,
    allowed: bool,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let Some(PendingRequest::Permission(request)) = handle.delegate.requests.take(request_id)
    else {
        return;
    };

    if allowed {
        request.allow();
    } else {
        request.deny();
    }
}

/// Answer a file picker previously reported through a
/// [`ServoFilePickerCallback`].
///
/// `paths` is an array of `path_count` NUL-terminated file paths the user
/// chose. A NULL `paths` or a `path_count` of 0 means the picker was dismissed
/// with no selection. Paths that are not valid UTF-8 are skipped; if that
/// leaves nothing, the picker is dismissed rather than submitted empty.
///
/// An unknown `request_id` — one already answered, or withdrawn by Servo — is
/// ignored.
///
/// # Safety
/// `webview` must be a valid handle. When `path_count` is non-zero, `paths`
/// must point to that many valid NUL-terminated C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_file_picker_respond(
    webview: *mut ServoWebViewHandle,
    request_id: u64,
    paths: *const *const c_char,
    path_count: usize,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let Some(PendingRequest::FilePicker(mut picker)) = handle.delegate.requests.take(request_id)
    else {
        return;
    };

    if paths.is_null() || path_count == 0 {
        picker.dismiss();
        return;
    }

    let selected: Vec<PathBuf> = unsafe { std::slice::from_raw_parts(paths, path_count) }
        .iter()
        .filter(|path| !path.is_null())
        .filter_map(|path| unsafe { CStr::from_ptr(*path) }.to_str().ok())
        .map(PathBuf::from)
        .collect();

    if selected.is_empty() {
        picker.dismiss();
        return;
    }

    picker.select(&selected);
    picker.submit();
}

/// Answer a dialog previously reported through a [`ServoDialogCallback`].
///
/// `accepted` is whether the user pressed the affirmative button; it is ignored
/// for alerts, which have only one. `text` is the prompt's entered text and is
/// ignored for alerts and confirms; passing NULL keeps the default that was
/// reported with the dialog.
///
/// An unknown `request_id` — one already answered, or withdrawn by Servo — is
/// ignored, so a host racing a withdrawal against a click does not need to
/// track which happened first.
///
/// # Safety
/// `webview` must be a valid handle and `text` NULL or a valid NUL-terminated
/// C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_dialog_respond(
    webview: *mut ServoWebViewHandle,
    request_id: u64,
    accepted: bool,
    text: *const c_char,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let Some(PendingRequest::SimpleDialog(dialog)) = handle.delegate.requests.take(request_id)
    else {
        return;
    };

    if !accepted {
        dialog.dismiss();
        return;
    }

    // Only a prompt carries text back to the page; for the others `confirm()`
    // is the whole answer.
    match dialog {
        SimpleDialog::Prompt(mut prompt) => {
            if !text.is_null()
                && let Ok(text) = unsafe { CStr::from_ptr(text) }.to_str()
            {
                prompt.set_current_value(text);
            }
            prompt.confirm();
        },
        dialog => dialog.confirm(),
    }
}

/// Begin loading `uri`. Invalid URLs are ignored.
///
/// # Safety
/// `webview` must be a valid handle and `uri` a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_load_uri(
    webview: *mut ServoWebViewHandle,
    uri: *const c_char,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    if uri.is_null() {
        return;
    }
    if let Ok(uri) = unsafe { CStr::from_ptr(uri) }.to_str() {
        if let Ok(url) = Url::parse(uri) {
            handle.webview.load(url);
        }
    }
}

/// Reload the current page.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_reload(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle.webview.reload();
    }
}

/// Navigate one entry back in session history.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_go_back(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        let _ = handle.webview.go_back(1);
    }
}

/// Navigate one entry forward in session history.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_go_forward(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        let _ = handle.webview.go_forward(1);
    }
}

/// Resize the webview surface to `width` x `height` device pixels.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_resize(
    webview: *mut ServoWebViewHandle,
    width: u32,
    height: u32,
) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle
            .webview
            .resize(dpi::PhysicalSize::new(width.max(1), height.max(1)));
    }
}

/// Set the HiDPI scale factor: the number of device pixels per
/// device-independent (logical) pixel, e.g. `2.0` on a doubled display.
///
/// The surface size passed to [`servo_webview_new`] and [`servo_webview_resize`]
/// is always in *device* pixels; this scale is what tells the page how large a
/// CSS pixel is, so `window.devicePixelRatio`, media queries and layout come
/// out right instead of the page being laid out at half size and upscaled.
///
/// Non-finite or non-positive values are ignored.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_hidpi_scale_factor(
    webview: *mut ServoWebViewHandle,
    scale: f32,
) {
    if !scale.is_finite() || scale <= 0.0 {
        return;
    }
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle.webview.set_hidpi_scale_factor(Scale::new(scale));
    }
}

/// Set the page zoom level: 1.0 is unzoomed, 2.0 is double size. This is the
/// zoom a browser's Ctrl+/Ctrl- applies — it changes the page's
/// `devicePixelRatio` and makes it re-lay out, rather than just magnifying the
/// rendered result.
///
/// Servo clamps the value to the inclusive range [0.1, 10.0]. Non-finite values
/// are ignored.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_zoom_level(
    webview: *mut ServoWebViewHandle,
    zoom: f32,
) {
    if !zoom.is_finite() || zoom <= 0.0 {
        return;
    }
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle.webview.set_page_zoom(zoom);
    }
}

/// Return the current page zoom level. An invalid handle reports 1.0.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_get_zoom_level(webview: *mut ServoWebViewHandle) -> f32 {
    match unsafe { as_handle(webview) } {
        Some(handle) => handle.webview.page_zoom(),
        None => 1.0,
    }
}

/// Report pointer movement to `(x, y)` in device pixels.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_pointer_move(
    webview: *mut ServoWebViewHandle,
    x: f64,
    y: f64,
) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle
            .webview
            .notify_input_event(InputEvent::MouseMove(MouseMoveEvent::new(
                WebViewPoint::Device(Point2D::new(x as f32, y as f32)),
            )));
    }
}

/// Report a pointer button press (`pressed != 0`) or release. `button` follows
/// GDK numbering: 1 = left, 2 = middle, 3 = right.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_pointer_button(
    webview: *mut ServoWebViewHandle,
    button: u32,
    pressed: bool,
    x: f64,
    y: f64,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let mouse_button = match button {
        2 => MouseButton::Middle,
        3 => MouseButton::Right,
        _ => MouseButton::Left,
    };
    let action = if pressed {
        MouseButtonAction::Down
    } else {
        MouseButtonAction::Up
    };
    handle
        .webview
        .notify_input_event(InputEvent::MouseButton(MouseButtonEvent::new(
            action,
            mouse_button,
            WebViewPoint::Device(Point2D::new(x as f32, y as f32)),
        )));
}

/// Scroll by `(dx, dy)` wheel deltas.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_scroll(
    webview: *mut ServoWebViewHandle,
    dx: f64,
    dy: f64,
) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        // The 20.0 multiplier matches Servo's winit_minimal example.
        handle.webview.notify_scroll_event(
            Scroll::Delta(WebViewVector::Device(DeviceVector2D::new(
                20.0 * dx as f32,
                20.0 * dy as f32,
            ))),
            WebViewPoint::Device(Point2D::new(10.0, 10.0)),
        );
    }
}

/// Named-key identifiers understood by [`servo_webview_key`]. These mirror the
/// `ServoKey` enum in `servo-webview.h` — keep the two in sync. Values other
/// than `CHARACTER` map to a [`NamedKey`]; `CHARACTER` means "use the Unicode
/// codepoint instead".
mod servo_key {
    pub const CHARACTER: u32 = 0;
    pub const UNIDENTIFIED: u32 = 1;
    pub const ENTER: u32 = 2;
    pub const TAB: u32 = 3;
    pub const BACKSPACE: u32 = 4;
    pub const DELETE: u32 = 5;
    pub const ESCAPE: u32 = 6;
    pub const ARROW_LEFT: u32 = 7;
    pub const ARROW_RIGHT: u32 = 8;
    pub const ARROW_UP: u32 = 9;
    pub const ARROW_DOWN: u32 = 10;
    pub const HOME: u32 = 11;
    pub const END: u32 = 12;
    pub const PAGE_UP: u32 = 13;
    pub const PAGE_DOWN: u32 = 14;
    pub const INSERT: u32 = 15;
    pub const F1: u32 = 16;
    pub const F2: u32 = 17;
    pub const F3: u32 = 18;
    pub const F4: u32 = 19;
    pub const F5: u32 = 20;
    pub const F6: u32 = 21;
    pub const F7: u32 = 22;
    pub const F8: u32 = 23;
    pub const F9: u32 = 24;
    pub const F10: u32 = 25;
    pub const F11: u32 = 26;
    pub const F12: u32 = 27;
}

/// Modifier bits understood by [`servo_webview_key`]. Mirrors the
/// `SERVO_MODIFIER_*` flags in `servo-webview.h`.
mod servo_modifier {
    pub const SHIFT: u32 = 1 << 0;
    pub const CONTROL: u32 = 1 << 1;
    pub const ALT: u32 = 1 << 2;
    pub const META: u32 = 1 << 3;
}

/// Report a key press (`pressed == true`) or release to the webview.
///
/// * `key` — a `ServoKey` value. Named keys (Enter, Tab, arrows, …) map to the
///   corresponding [`NamedKey`]; `SERVO_KEY_CHARACTER` (0) means the key
///   produced text, carried in `unicode`.
/// * `unicode` — the Unicode codepoint of the typed character when `key` is
///   `SERVO_KEY_CHARACTER`, otherwise ignored (pass 0).
/// * `modifiers` — a bitmask of `SERVO_MODIFIER_*` flags.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_key(
    webview: *mut ServoWebViewHandle,
    key: u32,
    unicode: u32,
    modifiers: u32,
    pressed: bool,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };

    let logical_key = match key {
        servo_key::ENTER => Key::Named(NamedKey::Enter),
        servo_key::TAB => Key::Named(NamedKey::Tab),
        servo_key::BACKSPACE => Key::Named(NamedKey::Backspace),
        servo_key::DELETE => Key::Named(NamedKey::Delete),
        servo_key::ESCAPE => Key::Named(NamedKey::Escape),
        servo_key::ARROW_LEFT => Key::Named(NamedKey::ArrowLeft),
        servo_key::ARROW_RIGHT => Key::Named(NamedKey::ArrowRight),
        servo_key::ARROW_UP => Key::Named(NamedKey::ArrowUp),
        servo_key::ARROW_DOWN => Key::Named(NamedKey::ArrowDown),
        servo_key::HOME => Key::Named(NamedKey::Home),
        servo_key::END => Key::Named(NamedKey::End),
        servo_key::PAGE_UP => Key::Named(NamedKey::PageUp),
        servo_key::PAGE_DOWN => Key::Named(NamedKey::PageDown),
        servo_key::INSERT => Key::Named(NamedKey::Insert),
        servo_key::F1 => Key::Named(NamedKey::F1),
        servo_key::F2 => Key::Named(NamedKey::F2),
        servo_key::F3 => Key::Named(NamedKey::F3),
        servo_key::F4 => Key::Named(NamedKey::F4),
        servo_key::F5 => Key::Named(NamedKey::F5),
        servo_key::F6 => Key::Named(NamedKey::F6),
        servo_key::F7 => Key::Named(NamedKey::F7),
        servo_key::F8 => Key::Named(NamedKey::F8),
        servo_key::F9 => Key::Named(NamedKey::F9),
        servo_key::F10 => Key::Named(NamedKey::F10),
        servo_key::F11 => Key::Named(NamedKey::F11),
        servo_key::F12 => Key::Named(NamedKey::F12),
        servo_key::UNIDENTIFIED => Key::Named(NamedKey::Unidentified),
        // CHARACTER (and any unknown value) falls back to the codepoint, then
        // to Unidentified if it isn't a valid scalar value.
        _ => match char::from_u32(unicode) {
            Some(c) if key == servo_key::CHARACTER => Key::Character(c.to_string()),
            _ => Key::Named(NamedKey::Unidentified),
        },
    };

    let mut mods = Modifiers::empty();
    if modifiers & servo_modifier::SHIFT != 0 {
        mods |= Modifiers::SHIFT;
    }
    if modifiers & servo_modifier::CONTROL != 0 {
        mods |= Modifiers::CONTROL;
    }
    if modifiers & servo_modifier::ALT != 0 {
        mods |= Modifiers::ALT;
    }
    if modifiers & servo_modifier::META != 0 {
        mods |= Modifiers::META;
    }

    let state = if pressed { KeyState::Down } else { KeyState::Up };

    handle
        .webview
        .notify_input_event(InputEvent::Keyboard(KeyboardEvent::new_without_event(
            state,
            logical_key,
            Code::Unidentified,
            Location::Standard,
            mods,
            false, // repeat
            false, // is_composing
        )));
}

/// Pump Servo's event loop once. Call this regularly from a GTK tick/timeout
/// source on the main thread; the frame-ready callback fires from inside it.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_spin(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle.servo.spin_event_loop();
    }
}

/// Return the currently loaded URL as a newly-allocated UTF-8 C string, or NULL
/// if none. Free the result with [`servo_string_free`].
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_get_uri(
    webview: *mut ServoWebViewHandle,
) -> *mut c_char {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return ptr::null_mut();
    };
    match handle.webview.url() {
        Some(url) => match CString::new(url.as_str()) {
            Ok(cstring) => cstring.into_raw(),
            Err(_) => ptr::null_mut(),
        },
        None => ptr::null_mut(),
    }
}

/// Return the current page title as a newly-allocated UTF-8 C string, or NULL
/// if the page has no title. Free the result with [`servo_string_free`].
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_get_title(
    webview: *mut ServoWebViewHandle,
) -> *mut c_char {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return ptr::null_mut();
    };
    match handle.webview.page_title() {
        Some(title) => match CString::new(title) {
            Ok(cstring) => cstring.into_raw(),
            Err(_) => ptr::null_mut(),
        },
        None => ptr::null_mut(),
    }
}

/// Return the current load status as a `SERVO_LOAD_STATUS_*` value. An invalid
/// handle reports `SERVO_LOAD_STATUS_COMPLETE` (nothing is loading).
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_get_load_status(
    webview: *mut ServoWebViewHandle,
) -> u32 {
    match unsafe { as_handle(webview) } {
        Some(handle) => load_status_to_abi(handle.webview.load_status()),
        None => servo_load_status::COMPLETE,
    }
}

/// Whether there is a previous entry in the session history to go back to.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_can_go_back(webview: *mut ServoWebViewHandle) -> bool {
    unsafe { as_handle(webview) }.is_some_and(|handle| handle.webview.can_go_back())
}

/// Whether there is a following entry in the session history to go forward to.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_can_go_forward(webview: *mut ServoWebViewHandle) -> bool {
    unsafe { as_handle(webview) }.is_some_and(|handle| handle.webview.can_go_forward())
}

/// Free a string previously returned by this library (e.g.
/// [`servo_webview_get_uri`]). Passing NULL is a no-op.
///
/// # Safety
/// `string` must be NULL or a pointer returned by this library and not yet
/// freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_string_free(string: *mut c_char) {
    if !string.is_null() {
        drop(unsafe { CString::from_raw(string) });
    }
}

/// Called once when an [`servo_webview_evaluate_script`] evaluation finishes.
///
/// Exactly one of `result_json` / `error` is non-NULL: on success `result_json`
/// is the return value serialized as a JSON string, on failure `error` is a
/// human-readable message. Both pointers are valid only for the duration of the
/// call — copy anything you need before returning.
pub type ServoScriptResultCallback = extern "C" fn(
    result_json: *const c_char,
    error: *const c_char,
    user_data: *mut c_void,
);

/// Convert Servo's [`JSValue`] into a plain [`serde_json::Value`].
///
/// `JSValue` derives serde `Serialize`, but that produces an externally-tagged
/// encoding (e.g. `{"Number":3.0}`); the C side wants the natural JSON shape
/// (`3`, `"foo"`, `[…]`), so map the variants explicitly. Node-reference
/// variants (Element, ShadowRoot, …) carry an opaque id string and are surfaced
/// as that string.
fn jsvalue_to_json(value: &JSValue) -> serde_json::Value {
    use serde_json::Value;
    match value {
        JSValue::Undefined | JSValue::Null => Value::Null,
        JSValue::Boolean(b) => Value::Bool(*b),
        JSValue::Number(n) => serde_json::Number::from_f64(*n)
            .map(Value::Number)
            .unwrap_or(Value::Null),
        JSValue::String(s)
        | JSValue::Element(s)
        | JSValue::ShadowRoot(s)
        | JSValue::Frame(s)
        | JSValue::Window(s) => Value::String(s.clone()),
        JSValue::Array(items) => Value::Array(items.iter().map(jsvalue_to_json).collect()),
        JSValue::Object(map) => Value::Object(
            map.iter()
                .map(|(k, v)| (k.clone(), jsvalue_to_json(v)))
                .collect(),
        ),
    }
}

/// A human-readable, one-line message for a [`JavaScriptEvaluationError`].
fn javascript_error_message(error: &JavaScriptEvaluationError) -> String {
    match error {
        JavaScriptEvaluationError::DocumentNotFound => {
            "the document that would run the script no longer exists".to_string()
        }
        JavaScriptEvaluationError::CompilationFailure => {
            "the script could not be compiled".to_string()
        }
        JavaScriptEvaluationError::EvaluationFailure(info) => match info {
            Some(info) => format!("uncaught exception: {}", info.message),
            None => "the script threw an uncaught exception".to_string(),
        },
        JavaScriptEvaluationError::InternalError => "internal Servo error".to_string(),
        JavaScriptEvaluationError::WebViewNotReady => "the web view is not ready".to_string(),
        JavaScriptEvaluationError::SerializationError(_) => {
            "the result could not be serialized".to_string()
        }
    }
}

/// Evaluate `script` as JavaScript in the webview's top-level browsing context.
///
/// Evaluation is asynchronous: `callback` is invoked exactly once, later, from
/// inside a [`servo_webview_spin`] call on the same thread. On success it
/// receives the return value serialized as a JSON string; on failure it
/// receives an error message. `user_data` is passed back verbatim. A NULL
/// `callback` is a no-op.
///
/// # Safety
/// `webview` must be a valid handle and `script` a valid NUL-terminated C
/// string. `user_data` is stored verbatim and handed back to the callback; the
/// caller owns its lifetime and must keep it valid until the callback fires.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_evaluate_script(
    webview: *mut ServoWebViewHandle,
    script: *const c_char,
    callback: Option<ServoScriptResultCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };

    let Some(callback) = callback else {
        return;
    };

    if script.is_null() {
        let error = CString::new("script is NULL").unwrap();
        callback(ptr::null(), error.as_ptr(), user_data);
        return;
    }

    let script = match unsafe { CStr::from_ptr(script) }.to_str() {
        Ok(script) => script,
        Err(_) => {
            let error = CString::new("script is not valid UTF-8").unwrap();
            callback(ptr::null(), error.as_ptr(), user_data);
            return;
        }
    };

    // `evaluate_javascript` converts the &str to an owned String synchronously
    // (before returning), so the borrow does not need to outlive this call. The
    // result closure is 'static and captures only the C function pointer and the
    // opaque user_data; it fires from a later spin_event_loop() on this thread.
    handle.webview.evaluate_javascript(script, move |result| {
        match result {
            Ok(value) => {
                let json = serde_json::to_string(&jsvalue_to_json(&value))
                    .unwrap_or_else(|_| "null".to_string());
                match CString::new(json) {
                    Ok(json) => callback(json.as_ptr(), ptr::null(), user_data),
                    Err(_) => {
                        // A NUL byte in the JSON (only possible inside a string
                        // value) can't be passed as a C string; report it.
                        let error =
                            CString::new("result contained an interior NUL byte").unwrap();
                        callback(ptr::null(), error.as_ptr(), user_data);
                    }
                }
            }
            Err(err) => {
                let error = CString::new(javascript_error_message(&err))
                    .unwrap_or_else(|_| CString::new("JavaScript evaluation failed").unwrap());
                callback(ptr::null(), error.as_ptr(), user_data);
            }
        }
    });
}
