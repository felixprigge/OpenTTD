/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file dropdown.cpp Implementation of the dropdown widget. */

#include "../stdafx.h"
#include "../window_gui.h"
#include "../string_func.h"
#include "../strings_func.h"
#include "../window_func.h"
#include "../zoom_func.h"
#include "../timer/timer.h"
#include "../timer/timer_window.h"
#include "dropdown_type.h"

#include "dropdown_widget.h"

#include "../safeguards.h"


static const NWidgetPart _nested_dropdown_menu_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_END, WID_DM_ITEMS), SetMinimalSize(1, 1), SetScrollbar(WID_DM_SCROLL), EndContainer(),
		NWidget(NWID_SELECTION, INVALID_COLOUR, WID_DM_SHOW_SCROLL),
			NWidget(NWID_VSCROLLBAR, COLOUR_END, WID_DM_SCROLL),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _dropdown_desc(__FILE__, __LINE__,
	WDP_MANUAL, nullptr, 0, 0,
	WC_DROPDOWN_MENU, WC_NONE,
	WDF_NO_FOCUS,
	std::begin(_nested_dropdown_menu_widgets), std::end(_nested_dropdown_menu_widgets)
);

/** Drop-down menu window */
struct DropdownWindow : Window {
	int parent_button;            ///< Parent widget number where the window is dropped from.
	const DropDownList list;      ///< List with dropdown menu items.
	int selected_result;          ///< Result value of the selected item in the list.
	byte click_delay;             ///< Timer to delay selection.
	bool drag_mode;
	bool instant_close;           ///< Close the window when the mouse button is raised.
	int scrolling;                ///< If non-zero, auto-scroll the item list (one time).
	Point position;               ///< Position of the topleft corner of the window.
	Scrollbar *vscroll;

	/**
	 * Create a dropdown menu.
	 * @param parent        Parent window.
	 * @param list          Dropdown item list.
	 * @param selected      Initial selected result of the list.
	 * @param button        Widget of the parent window doing the dropdown.
	 * @param instant_close Close the window when the mouse button is raised.
	 * @param position      Topleft position of the dropdown menu window.
	 * @param size          Size of the dropdown menu window.
	 * @param wi_colour     Colour of the parent widget.
	 * @param scroll        Dropdown menu has a scrollbar.
	 */
	DropdownWindow(Window *parent, DropDownList &&list, int selected, int button, bool instant_close, const Point &position, const Dimension &size, Colours wi_colour, bool scroll)
			: Window(&_dropdown_desc), list(std::move(list))
	{
		assert(!this->list.empty());

		this->position = position;

		this->CreateNestedTree();

		this->vscroll = this->GetScrollbar(WID_DM_SCROLL);

		uint items_width = size.width - (scroll ? NWidgetScrollbar::GetVerticalDimension().width : 0);
		NWidgetCore *nwi = this->GetWidget<NWidgetCore>(WID_DM_ITEMS);
		nwi->SetMinimalSizeAbsolute(items_width, size.height + WidgetDimensions::scaled.dropdownlist.Vertical());
		nwi->colour = wi_colour;

		nwi = this->GetWidget<NWidgetCore>(WID_DM_SCROLL);
		nwi->colour = wi_colour;

		this->GetWidget<NWidgetStacked>(WID_DM_SHOW_SCROLL)->SetDisplayedPlane(scroll ? 0 : SZSP_NONE);

		this->FinishInitNested(0);
		CLRBITS(this->flags, WF_WHITE_BORDER);

		/* Total length of list */
		int list_height = 0;
		for (const auto &item : this->list) {
			list_height += item->Height();
		}

		/* Capacity is the average number of items visible */
		this->vscroll->SetCapacity(size.height * this->list.size() / list_height);
		this->vscroll->SetCount(this->list.size());

		this->parent           = parent;
		this->parent_button    = button;
		this->selected_result   = selected;
		this->click_delay      = 0;
		this->drag_mode        = true;
		this->instant_close    = instant_close;
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		/* Finish closing the dropdown, so it doesn't affect new window placement.
		 * Also mark it dirty in case the callback deals with the screen. (e.g. screenshots). */
		this->Window::Close();

		Point pt = _cursor.pos;
		pt.x -= this->parent->left;
		pt.y -= this->parent->top;
		this->parent->OnDropdownClose(pt, this->parent_button, this->selected_result, this->instant_close);

		/* Set flag on parent widget to indicate that we have just closed. */
		NWidgetCore *nwc = this->parent->GetWidget<NWidgetCore>(this->parent_button);
		if (nwc != nullptr) SetBit(nwc->disp_flags, NDB_DROPDOWN_CLOSED);
	}

	void OnFocusLost(bool closing) override
	{
		if (!closing) {
			this->instant_close = false;
			this->Close();
		}
	}

	Point OnInitialPosition([[maybe_unused]] int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return this->position;
	}

	/**
	 * Find the dropdown item under the cursor.
	 * @param[out] value Selected item, if function returns \c true.
	 * @return Cursor points to a dropdown item.
	 */
	bool GetDropDownItem(int &value)
	{
		if (GetWidgetFromPos(this, _cursor.pos.x - this->left, _cursor.pos.y - this->top) < 0) return false;

		const Rect &r = this->GetWidget<NWidgetBase>(WID_DM_ITEMS)->GetCurrentRect().Shrink(WidgetDimensions::scaled.dropdownlist);
		int y     = _cursor.pos.y - this->top - r.top;
		int pos   = this->vscroll->GetPosition();

		for (const auto &item : this->list) {
			/* Skip items that are scrolled up */
			if (--pos >= 0) continue;

			int item_height = item->Height();

			if (y < item_height) {
				if (item->masked || !item->Selectable()) return false;
				value = item->result;
				return true;
			}

			y -= item_height;
		}

		return false;
	}

	void DrawWidget(const Rect &r, int widget) const override
	{
		if (widget != WID_DM_ITEMS) return;

		Colours colour = this->GetWidget<NWidgetCore>(widget)->colour;

		Rect ir = r.Shrink(WidgetDimensions::scaled.dropdownlist);
		int y = ir.top;
		int pos = this->vscroll->GetPosition();
		for (const auto &item : this->list) {
			int item_height = item->Height();

			/* Skip items that are scrolled up */
			if (--pos >= 0) continue;

			if (y + item_height - 1 <= ir.bottom) {
				Rect full{ir.left, y, ir.right, y + item_height - 1};

				bool selected = (this->selected_result == item->result) && item->Selectable();
				if (selected) GfxFillRect(full, PC_BLACK);

				item->Draw(full, full.Shrink(WidgetDimensions::scaled.dropdowntext, RectPadding::zero), selected, colour);
			}
			y += item_height;
		}
	}

	void OnClick([[maybe_unused]] Point pt, int widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_DM_ITEMS) return;
		int item;
		if (this->GetDropDownItem(item)) {
			this->click_delay = 4;
			this->selected_result = item;
			this->SetDirty();
		}
	}

	/** Rate limit how fast scrolling happens. */
	IntervalTimer<TimerWindow> scroll_interval = {std::chrono::milliseconds(30), [this](auto) {
		if (this->scrolling == 0) return;

		if (this->vscroll->UpdatePosition(this->scrolling)) this->SetDirty();

		this->scrolling = 0;
	}};

	void OnMouseLoop() override
	{
		if (this->click_delay != 0 && --this->click_delay == 0) {
			/* Close the dropdown, so it doesn't affect new window placement.
			 * Also mark it dirty in case the callback deals with the screen. (e.g. screenshots). */
			this->Close();
			this->parent->OnDropdownSelect(this->parent_button, this->selected_result);
			return;
		}

		if (this->drag_mode) {
			int item;

			if (!_left_button_clicked) {
				this->drag_mode = false;
				if (!this->GetDropDownItem(item)) {
					if (this->instant_close) this->Close();
					return;
				}
				this->click_delay = 2;
			} else {
				if (_cursor.pos.y <= this->top + 2) {
					/* Cursor is above the list, set scroll up */
					this->scrolling = -1;
					return;
				} else if (_cursor.pos.y >= this->top + this->height - 2) {
					/* Cursor is below list, set scroll down */
					this->scrolling = 1;
					return;
				}

				if (!this->GetDropDownItem(item)) return;
			}

			if (this->selected_result != item) {
				this->selected_result = item;
				this->SetDirty();
			}
		}
	}
};

/**
 * Determine width and height required to fully display a DropDownList
 * @param list The list.
 * @return Dimension required to display the list.
 */
Dimension GetDropDownListDimension(const DropDownList &list)
{
	Dimension dim{};
	for (const auto &item : list) {
		dim.height += item->Height();
		dim.width = std::max(dim.width, item->Width());
	}
	dim.width += WidgetDimensions::scaled.dropdowntext.Horizontal();
	return dim;
}

/**
 * Show a drop down list.
 * @param w        Parent window for the list.
 * @param list     Prepopulated DropDownList.
 * @param selected The initially selected list item.
 * @param button   The widget which is passed to Window::OnDropdownSelect and OnDropdownClose.
 *                 Unless you override those functions, this should be then widget index of the dropdown button.
 * @param wi_rect  Coord of the parent drop down button, used to position the dropdown menu.
 * @param instant_close Set to true if releasing mouse button should close the
 *                      list regardless of where the cursor is.
 */
void ShowDropDownListAt(Window *w, DropDownList &&list, int selected, int button, Rect wi_rect, Colours wi_colour, bool instant_close)
{
	CloseWindowByClass(WC_DROPDOWN_MENU);

	/* The preferred position is just below the dropdown calling widget */
	int top = w->top + wi_rect.bottom + 1;

	/* The preferred width equals the calling widget */
	uint width = wi_rect.Width();

	/* Get the height and width required for the list. */
	Dimension dim = GetDropDownListDimension(list);
	dim.width += WidgetDimensions::scaled.dropdownlist.Horizontal();

	/* Scrollbar needed? */
	bool scroll = false;

	/* Is it better to place the dropdown above the widget? */
	bool above = false;

	/* Available height below (or above, if the dropdown is placed above the widget). */
	uint available_height = std::max(GetMainViewBottom() - top - (int)WidgetDimensions::scaled.dropdownlist.Vertical(), 0);

	/* If the dropdown doesn't fully fit below the widget... */
	if (dim.height > available_height) {

		uint available_height_above = std::max(w->top + wi_rect.top - GetMainViewTop() - (int)WidgetDimensions::scaled.dropdownlist.Vertical(), 0);

		/* Put the dropdown above if there is more available space. */
		if (available_height_above > available_height) {
			above = true;
			available_height = available_height_above;
		}

		/* If the dropdown doesn't fully fit, we need a dropdown. */
		if (dim.height > available_height) {
			scroll = true;
			uint avg_height = dim.height / (uint)list.size();

			/* Fit the list; create at least one row, even if there is no height available. */
			uint rows = std::max<uint>(available_height / avg_height, 1);
			dim.height = rows * avg_height;

			/* Add space for the scrollbar. */
			dim.width += NWidgetScrollbar::GetVerticalDimension().width;
		}

		/* Set the top position if needed. */
		if (above) {
			top = w->top + wi_rect.top - dim.height - WidgetDimensions::scaled.dropdownlist.Vertical();
		}
	}

	dim.width = std::max(width, dim.width);

	Point dw_pos = { w->left + (_current_text_dir == TD_RTL ? wi_rect.right + 1 - (int)width : wi_rect.left), top};
	DropdownWindow *dropdown = new DropdownWindow(w, std::move(list), selected, button, instant_close, dw_pos, dim, wi_colour, scroll);

	/* The dropdown starts scrolling downwards when opening it towards
	 * the top and holding down the mouse button. It can be fooled by
	 * opening the dropdown scrolled to the very bottom.  */
	if (above && scroll) dropdown->vscroll->UpdatePosition(INT_MAX);
}

/**
 * Show a drop down list.
 * @param w        Parent window for the list.
 * @param list     Prepopulated DropDownList.
 * @param selected The initially selected list item.
 * @param button   The widget within the parent window that is used to determine
 *                 the list's location.
 * @param width    Override the minimum width determined by the selected widget and list contents.
 * @param instant_close Set to true if releasing mouse button should close the
 *                      list regardless of where the cursor is.
 */
void ShowDropDownList(Window *w, DropDownList &&list, int selected, int button, uint width, bool instant_close)
{
	/* Our parent's button widget is used to determine where to place the drop
	 * down list window. */
	NWidgetCore *nwi = w->GetWidget<NWidgetCore>(button);
	Rect wi_rect      = nwi->GetCurrentRect();
	Colours wi_colour = nwi->colour;

	if ((nwi->type & WWT_MASK) == NWID_BUTTON_DROPDOWN) {
		nwi->disp_flags |= ND_DROPDOWN_ACTIVE;
	} else {
		nwi->SetLowered(true);
	}
	nwi->SetDirty(w);

	if (width != 0) {
		if (_current_text_dir == TD_RTL) {
			wi_rect.left = wi_rect.right + 1 - ScaleGUITrad(width);
		} else {
			wi_rect.right = wi_rect.left + ScaleGUITrad(width) - 1;
		}
	}

	ShowDropDownListAt(w, std::move(list), selected, button, wi_rect, wi_colour, instant_close);
}

/**
 * Show a dropdown menu window near a widget of the parent window.
 * The result code of the items is their index in the \a strings list.
 * @param w             Parent window that wants the dropdown menu.
 * @param strings       Menu list, end with #INVALID_STRING_ID
 * @param selected      Index of initial selected item.
 * @param button        Button widget number of the parent window \a w that wants the dropdown menu.
 * @param disabled_mask Bitmask for disabled items (items with their bit set are displayed, but not selectable in the dropdown list).
 * @param hidden_mask   Bitmask for hidden items (items with their bit set are not copied to the dropdown list).
 * @param width         Minimum width of the dropdown menu.
 */
void ShowDropDownMenu(Window *w, const StringID *strings, int selected, int button, uint32_t disabled_mask, uint32_t hidden_mask, uint width)
{
	DropDownList list;

	for (uint i = 0; strings[i] != INVALID_STRING_ID; i++) {
		if (!HasBit(hidden_mask, i)) {
			list.push_back(std::make_unique<DropDownListStringItem>(strings[i], i, HasBit(disabled_mask, i)));
		}
	}

	if (!list.empty()) ShowDropDownList(w, std::move(list), selected, button, width);
}
