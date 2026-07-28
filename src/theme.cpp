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

/// @file theme.cpp
/// @brief Light/dark UI appearance handling.
/// @ingroup main

#include "include/aegisub/theme.h"

#include "options.h"

#include <libaegisub/color.h>
#include <libaegisub/signal.h>

#include <wx/app.h>
#include <wx/settings.h>
#include <wx/toplevel.h>
#include <wx/window.h>

namespace {
	/// A colour option and its light/dark values. These are the surfaces that
	/// Aegisub draws itself, so wxWidgets' native dark mode does not touch them
	/// and we recolour them via the options they already read. The audio
	/// display, video overlays and visual tools keep their own palettes (they
	/// already render onto dark surfaces).
	struct ThemeColor {
		const char *option;
		const char *light;
		const char *dark;
	};

	constexpr ThemeColor palette[] = {
		// Subtitle grid
		{"Colour/Subtitle Grid/Standard",                     "rgb(0,0,0)",         "rgb(220,220,220)"},
		{"Colour/Subtitle Grid/Selection",                    "rgb(0,0,0)",         "rgb(235,235,235)"},
		{"Colour/Subtitle Grid/Collision",                    "rgb(255,0,0)",       "rgb(255,105,105)"},
		{"Colour/Subtitle Grid/Background/Background",         "rgb(255,255,255)",   "rgb(30,30,30)"},
		{"Colour/Subtitle Grid/Background/Selection",         "rgb(206,255,231)",   "rgb(38,66,54)"},
		{"Colour/Subtitle Grid/Background/Comment",           "rgb(216,222,245)",   "rgb(45,48,66)"},
		{"Colour/Subtitle Grid/Background/Selected Comment",  "rgb(211,238,238)",   "rgb(38,60,60)"},
		{"Colour/Subtitle Grid/Background/Inframe",           "rgb(255,253,234)",   "rgb(58,56,36)"},
		{"Colour/Subtitle Grid/Header",                       "rgb(165,207,231)",   "rgb(50,55,62)"},
		{"Colour/Subtitle Grid/Left Column",                  "rgb(196,236,201)",   "rgb(40,52,42)"},
		{"Colour/Subtitle Grid/Lines",                        "rgb(190,190,190)",   "rgb(70,70,70)"},
		{"Colour/Subtitle Grid/Active Border",                "rgb(255,91,239)",    "rgb(255,120,242)"},
		{"Colour/Subtitle Grid/CPS Error",                    "rgb(255,0,0)",       "rgb(120,0,0)"},

		// Syntax-highlighted edit box
		{"Colour/Subtitle/Background",                        "rgb(255,255,255)",   "rgb(30,30,30)"},
		{"Colour/Subtitle/Syntax/Normal",                     "rgb(0,0,0)",         "rgb(220,220,220)"},
		{"Colour/Subtitle/Syntax/Comment",                    "rgb(0,0,0)",         "rgb(150,150,150)"},
		{"Colour/Subtitle/Syntax/Drawing Command",            "rgb(0,0,0)",         "rgb(220,220,220)"},
		{"Colour/Subtitle/Syntax/Drawing X",                  "rgb(90,40,40)",      "rgb(214,142,142)"},
		{"Colour/Subtitle/Syntax/Drawing Y",                  "rgb(40,90,40)",      "rgb(142,214,142)"},
		{"Colour/Subtitle/Syntax/Brackets",                   "rgb(20,50,255)",     "rgb(120,150,255)"},
		{"Colour/Subtitle/Syntax/Slashes",                    "rgb(255,0,200)",     "rgb(255,120,220)"},
		{"Colour/Subtitle/Syntax/Tags",                       "rgb(90,90,90)",      "rgb(165,165,165)"},
		{"Colour/Subtitle/Syntax/Parameters",                 "rgb(40,90,40)",      "rgb(142,204,142)"},
		{"Colour/Subtitle/Syntax/Error",                      "rgb(200,0,0)",       "rgb(255,110,110)"},
		{"Colour/Subtitle/Syntax/Background/Error",           "rgb(255,200,200)",   "rgb(92,42,42)"},
		{"Colour/Subtitle/Syntax/Line Break",                 "rgb(160,160,160)",   "rgb(120,120,120)"},
		{"Colour/Subtitle/Syntax/Karaoke Template",           "rgb(128,0,192)",     "rgb(200,130,255)"},
		{"Colour/Subtitle/Syntax/Karaoke Variable",           "rgb(128,0,192)",     "rgb(200,130,255)"},

		// Style editor preview backdrop
		{"Colour/Style Editor/Background/Preview",            "rgb(125,153,176)",   "rgb(60,66,74)"},
	};

	agi::signal::Connection appearance_slot;

	theme::Appearance current_setting() {
		return static_cast<theme::Appearance>(OPT_GET("App/Appearance")->GetInt());
	}

	void ApplyPalette(bool dark) {
		for (auto const& c : palette)
			OPT_SET(c.option)->SetColor(agi::Color(dark ? c.dark : c.light));
	}

	// Ask wxWidgets to theme all native controls (menu bar, dialogs, popup
	// menus, buttons, scrollbars, title bar, ...). This is only available in
	// wxWidgets 3.3+; older versions fall back to the palette above plus
	// whatever the platform theme provides.
	void ApplyNativeAppearance() {
#if wxCHECK_VERSION(3, 3, 0)
		if (!wxTheApp) return;
		switch (current_setting()) {
			case theme::Appearance::Light:
				wxTheApp->SetAppearance(wxApp::Appearance::Light);
				break;
			case theme::Appearance::Dark:
				wxTheApp->SetAppearance(wxApp::Appearance::Dark);
				break;
			case theme::Appearance::System:
			default:
				wxTheApp->SetAppearance(wxApp::Appearance::System);
				break;
		}
#endif
	}

	void OnAppearanceChanged() {
		// Set the native appearance first so that IsDark() reflects it when we
		// resolve the palette for "System".
		ApplyNativeAppearance();
		ApplyPalette(theme::IsDark());
		for (wxWindow *w : wxTopLevelWindows)
			w->Refresh();
	}
}

namespace theme {
	bool IsDark() {
		switch (current_setting()) {
			case Appearance::Light: return false;
			case Appearance::Dark:  return true;
			case Appearance::System:
			default:
				return wxSystemSettings::GetAppearance().IsDark();
		}
	}

	void Init() {
		// Set the native appearance first so that IsDark() reflects it when we
		// resolve the palette for "System".
		ApplyNativeAppearance();
		ApplyPalette(IsDark());
		appearance_slot = OPT_SUB("App/Appearance", [](agi::OptionValue const&) { OnAppearanceChanged(); });
	}
}
