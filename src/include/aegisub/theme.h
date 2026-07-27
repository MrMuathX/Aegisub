// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file theme.h
/// @brief Light/dark UI appearance handling.
/// @ingroup main

class wxWindow;

namespace theme {
	/// Requested appearance, stored in the "App/Appearance" option.
	enum class Appearance {
		System = 0,
		Light  = 1,
		Dark   = 2,
	};

	/// Resolve whether the effective appearance is dark, taking the
	/// "System" setting and the OS appearance into account.
	bool IsDark();

	/// Apply the palette for the current "App/Appearance" setting to the
	/// colour options and subscribe to future changes. Call once after the
	/// options have been loaded and before any windows are created.
	void Init();

	/// Apply the current appearance to a top-level window and its children
	/// (background/foreground colours plus, on Windows, the dark title bar).
	/// Safe to call repeatedly; a no-op contribution on platforms that theme
	/// native controls themselves.
	void SetupWindow(wxWindow *window);
}
