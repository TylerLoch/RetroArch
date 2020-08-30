/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _MSC_VER
#pragma comment(lib, "dinput8")
#endif

#undef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800

#include <dinput.h>
#include <dbt.h>

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <boolean.h>

#include <windowsx.h>

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL                  0x20e
#endif

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL                   0x020A
#endif

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <string/stdstring.h>

#ifndef _XBOX
#include "../../gfx/common/win32_common.h"
#endif

#include "../input_keymaps.h"

#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

/* Context has to be global as joypads also ride on this context. */
LPDIRECTINPUT8 g_dinput_ctx;

struct pointer_status
{
   struct pointer_status *next;
   int pointer_id;
   int pointer_x;
   int pointer_y;
};

struct dinput_input
{
   char *joypad_driver_name;
   LPDIRECTINPUTDEVICE8 keyboard;
   LPDIRECTINPUTDEVICE8 mouse;
   const input_device_driver_t *joypad;
   struct pointer_status pointer_head;  /* dummy head for easier iteration */

   int window_pos_x;
   int window_pos_y;
   int mouse_rel_x;
   int mouse_rel_y;
   int mouse_x;
   int mouse_y;
   uint8_t state[256];
   bool doubleclick_on_titlebar;
   bool mouse_l, mouse_r, mouse_m, mouse_b4, mouse_b5, mouse_wu, mouse_wd, mouse_hwu, mouse_hwd;
};

void dinput_destroy_context(void)
{
   if (!g_dinput_ctx)
      return;

   IDirectInput8_Release(g_dinput_ctx);
   g_dinput_ctx = NULL;
}

bool dinput_init_context(void)
{
   if (g_dinput_ctx)
      return true;

   /* Who said we shouldn't have same call signature in a COM API? <_< */
#ifdef __cplusplus
   if (!(SUCCEEDED(DirectInput8Create(
                  GetModuleHandle(NULL), DIRECTINPUT_VERSION,
                  IID_IDirectInput8,
                  (void**)&g_dinput_ctx, NULL))))
#else
      if (!(SUCCEEDED(DirectInput8Create(
                     GetModuleHandle(NULL), DIRECTINPUT_VERSION,
                     &IID_IDirectInput8,
                     (void**)&g_dinput_ctx, NULL))))
#endif
      goto error;

   return true;

error:
   RARCH_ERR("[DINPUT]: Failed to initialize DirectInput.\n");
   return false;
}

static void *dinput_init(const char *joypad_driver)
{
   struct dinput_input *di = NULL;

   if (!dinput_init_context())
   {
      RARCH_ERR("[DINPUT]: Failed to start DirectInput driver.\n");
      return NULL;
   }

   di = (struct dinput_input*)calloc(1, sizeof(*di));
   if (!di)
      return NULL;

   if (!string_is_empty(joypad_driver))
      di->joypad_driver_name = strdup(joypad_driver);

#ifdef __cplusplus
   if (FAILED(IDirectInput8_CreateDevice(g_dinput_ctx,
               GUID_SysKeyboard,
               &di->keyboard, NULL)))
#else
   if (FAILED(IDirectInput8_CreateDevice(g_dinput_ctx,
               &GUID_SysKeyboard,
               &di->keyboard, NULL)))
#endif
   {
      RARCH_ERR("[DINPUT]: Failed to create keyboard device.\n");
      di->keyboard = NULL;
   }

#ifdef __cplusplus
   if (FAILED(IDirectInput8_CreateDevice(g_dinput_ctx,
               GUID_SysMouse,
               &di->mouse, NULL)))
#else
   if (FAILED(IDirectInput8_CreateDevice(g_dinput_ctx,
               &GUID_SysMouse,
               &di->mouse, NULL)))
#endif
   {
      RARCH_ERR("[DINPUT]: Failed to create mouse device.\n");
      di->mouse = NULL;
   }

   if (di->keyboard)
   {
      IDirectInputDevice8_SetDataFormat(di->keyboard, &c_dfDIKeyboard);
      IDirectInputDevice8_SetCooperativeLevel(di->keyboard,
            (HWND)video_driver_window_get(), DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
      IDirectInputDevice8_Acquire(di->keyboard);
   }

   if (di->mouse)
   {
      IDirectInputDevice8_SetDataFormat(di->mouse, &c_dfDIMouse2);
      IDirectInputDevice8_SetCooperativeLevel(di->mouse, (HWND)video_driver_window_get(),
            DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
      IDirectInputDevice8_Acquire(di->mouse);
   }

   input_keymaps_init_keyboard_lut(rarch_key_map_dinput);

#ifndef _XBOX
   win32_set_input_userdata(di);
#endif

   return di;
}

static void dinput_poll(void *data)
{
   struct dinput_input *di = (struct dinput_input*)data;
   uint8_t *kb_state       = NULL;

   if (!di)
      return;

   kb_state                = &di->state[0];

   for (
         ; kb_state < di->state + 256
         ; kb_state++)
      *kb_state = 0;

   if (di->keyboard)
   {
      if (FAILED(IDirectInputDevice8_GetDeviceState(
                  di->keyboard, sizeof(di->state), di->state)))
      {
         IDirectInputDevice8_Acquire(di->keyboard);
         if (FAILED(IDirectInputDevice8_GetDeviceState(
                     di->keyboard, sizeof(di->state), di->state)))
         {
            for (
                  ; kb_state < di->state + 256
                  ; kb_state++)
               *kb_state = 0;
         }
      }
   }

   if (di->mouse)
   {
      POINT point;
      DIMOUSESTATE2 mouse_state;
      BYTE *rgb_buttons_ptr     = &mouse_state.rgbButtons[0];
      
      point.x = 0;
      point.y = 0;

      mouse_state.lX = 0;
      mouse_state.lY = 0;
      mouse_state.lZ = 0;

      for (
            ; rgb_buttons_ptr < mouse_state.rgbButtons + 8
            ; rgb_buttons_ptr++)
         *rgb_buttons_ptr = 0;

      if (FAILED(IDirectInputDevice8_GetDeviceState(
                  di->mouse, sizeof(mouse_state), &mouse_state)))
      {
         IDirectInputDevice8_Acquire(di->mouse);
         if (FAILED(IDirectInputDevice8_GetDeviceState(
                     di->mouse, sizeof(mouse_state), &mouse_state)))
         {
            mouse_state.lX = 0;
            mouse_state.lY = 0;
            mouse_state.lZ = 0;
            for (
                  ; rgb_buttons_ptr < mouse_state.rgbButtons + 8
                  ; rgb_buttons_ptr++)
               *rgb_buttons_ptr = 0;
         }
      }

      di->mouse_rel_x = mouse_state.lX;
      di->mouse_rel_y = mouse_state.lY;

      if (!mouse_state.rgbButtons[0])
         di->doubleclick_on_titlebar = false;
      if (di->doubleclick_on_titlebar)
         di->mouse_l  = 0;
      else
         di->mouse_l  = mouse_state.rgbButtons[0];
      di->mouse_r     = mouse_state.rgbButtons[1];
      di->mouse_m     = mouse_state.rgbButtons[2];
      di->mouse_b4    = mouse_state.rgbButtons[3];
      di->mouse_b5    = mouse_state.rgbButtons[4];

      /* No simple way to get absolute coordinates
       * for RETRO_DEVICE_POINTER. Just use Win32 APIs. */
      GetCursorPos(&point);
      ScreenToClient((HWND)video_driver_window_get(), &point);
      di->mouse_x = point.x;
      di->mouse_y = point.y;
   }
}

static bool dinput_mouse_button_pressed(
      struct dinput_input *di, unsigned port, unsigned key)
{
	bool result;

	switch (key)
   {
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return di->mouse_l;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return di->mouse_r;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return di->mouse_m;
      case RETRO_DEVICE_ID_MOUSE_BUTTON_4:
         return di->mouse_b4;
      case RETRO_DEVICE_ID_MOUSE_BUTTON_5:
         return di->mouse_b5;

      case RETRO_DEVICE_ID_MOUSE_WHEELUP:
         result = di->mouse_wu;
         di->mouse_wu = false;
         return result;

      case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
         result = di->mouse_wd;
         di->mouse_wd = false;
         return result;

      case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP:
         result = di->mouse_hwu;
         di->mouse_hwu = false;
         return result;

      case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN:
         result = di->mouse_hwd;
         di->mouse_hwd = false;
         return result;
   }

	return false;
}

static int16_t dinput_pressed_analog(
      struct dinput_input *di,
      const struct retro_keybind *binds,
      unsigned idx, unsigned id)
{
   int id_minus_key      = 0;
   int id_plus_key       = 0;
   unsigned id_minus     = 0;
   unsigned id_plus      = 0;
   int16_t ret           = 0;
   bool id_plus_valid    = false;
   bool id_minus_valid   = false;

   input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

   id_minus_valid        = binds[id_minus].valid;
   id_plus_valid         = binds[id_plus].valid;
   id_minus_key          = binds[id_minus].key;
   id_plus_key           = binds[id_plus].key;

   if (!id_minus_valid || !id_plus_valid)
      return 0;

   if (id_plus_valid && id_plus_key < RETROK_LAST)
   {
      unsigned sym = rarch_keysym_lut[(enum retro_key)id_plus_key];
      if (di->state[sym] & 0x80)
         ret = 0x7fff;
   }
   if (id_minus_valid && id_minus_key < RETROK_LAST)
   {
      unsigned sym = rarch_keysym_lut[(enum retro_key)id_minus_key];
      if (di->state[sym] & 0x80)
         ret += -0x7fff;
   }

   return ret;
}

static int16_t dinput_lightgun_aiming_state(
      struct dinput_input *di, unsigned idx, unsigned id)
{
   struct video_viewport vp;
   const int edge_detect       = 32700;
   bool inside                 = false;
   int16_t res_x               = 0;
   int16_t res_y               = 0;
   int16_t res_screen_x        = 0;
   int16_t res_screen_y        = 0;

   int x                       = 0;
   int y                       = 0;
   unsigned num                = 0;

   struct pointer_status 
      *check_pos               = di->pointer_head.next;

   vp.x                        = 0;
   vp.y                        = 0;
   vp.width                    = 0;
   vp.height                   = 0;
   vp.full_width               = 0;
   vp.full_height              = 0;

   while (check_pos && num < idx)
   {
      num++;
      check_pos = check_pos->next;
   }

   if (!check_pos && idx > 0) /* idx = 0 has mouse fallback. */
      return 0;

   x = di->mouse_x;
   y = di->mouse_y;

   if (check_pos)
   {
      x = check_pos->pointer_x;
      y = check_pos->pointer_y;
   }

   if (!(video_driver_translate_coord_viewport_wrap(
               &vp, x, y,
               &res_x, &res_y, &res_screen_x, &res_screen_y)))
      return 0;

   inside =    (res_x >= -edge_detect) 
            && (res_y >= -edge_detect)
            && (res_x <= edge_detect)
            && (res_y <= edge_detect);

   switch ( id )
   {
      case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
         if (inside)
            return res_x;
         break;
      case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
         if (inside)
            return res_y;
         break;
      case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
         return !inside;
      default:
         break;
   }

   return 0;
}

static int16_t dinput_mouse_state(struct dinput_input *di,
      unsigned port, unsigned id)
{
   int16_t        state = 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_X:
         return di->mouse_rel_x;
      case RETRO_DEVICE_ID_MOUSE_Y:
         return di->mouse_rel_y;
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return di->mouse_l;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return di->mouse_r;
      case RETRO_DEVICE_ID_MOUSE_WHEELUP:
         if (di->mouse_wu)
            state = 1;
         di->mouse_wu = false;
         return state;
      case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
         if (di->mouse_wd)
            state = 1;
         di->mouse_wd = false;
         return state;
      case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP:
         if (di->mouse_hwu)
            state = 1;
         di->mouse_hwu = false;
         return state;
      case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN:
         if (di->mouse_hwd)
            state = 1;
         di->mouse_hwd = false;
         return state;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return di->mouse_m;
      case RETRO_DEVICE_ID_MOUSE_BUTTON_4:
         return di->mouse_b4;
      case RETRO_DEVICE_ID_MOUSE_BUTTON_5:
         return di->mouse_b5;
   }

   return 0;
}

static int16_t dinput_mouse_state_screen(struct dinput_input *di,
      unsigned port, unsigned id)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_X:
         return di->mouse_x;
      case RETRO_DEVICE_ID_MOUSE_Y:
         return di->mouse_y;
      default:
         break;
   }

   return dinput_mouse_state(di, port, id);
}

static int16_t dinput_pointer_state(struct dinput_input *di,
      unsigned idx, unsigned id, bool screen)
{
   struct video_viewport vp;
   bool pointer_down           = false;
   bool inside                 = false;
   int x                       = 0;
   int y                       = 0;
   int16_t res_x               = 0;
   int16_t res_y               = 0;
   int16_t res_screen_x        = 0;
   int16_t res_screen_y        = 0;
   unsigned num                = 0;
   struct pointer_status *
      check_pos                = di->pointer_head.next;

   vp.x                        = 0;
   vp.y                        = 0;
   vp.width                    = 0;
   vp.height                   = 0;
   vp.full_width               = 0;
   vp.full_height              = 0;

   while (check_pos && num < idx)
   {
      num++;
      check_pos    = check_pos->next;
   }
   if (!check_pos && idx > 0) /* idx = 0 has mouse fallback. */
      return 0;

   x               = di->mouse_x;
   y               = di->mouse_y;
   pointer_down    = di->mouse_l;

   if (check_pos)
   {
      x            = check_pos->pointer_x;
      y            = check_pos->pointer_y;
      pointer_down = true;
   }

   if (!(video_driver_translate_coord_viewport_wrap(&vp, x, y,
         &res_x, &res_y, &res_screen_x, &res_screen_y)))
      return 0;

   if (screen)
   {
      res_x        = res_screen_x;
      res_y        = res_screen_y;
   }

   inside = (res_x >= -0x7fff) && (res_y >= -0x7fff);

   if (!inside)
      return 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_POINTER_X:
         return res_x;
      case RETRO_DEVICE_ID_POINTER_Y:
         return res_y;
      case RETRO_DEVICE_ID_POINTER_PRESSED:
         return pointer_down;
      default:
         break;
   }

   return 0;
}

static int16_t dinput_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind **binds, unsigned port,
      unsigned device, unsigned idx, unsigned id)
{
   settings_t *settings;
   struct dinput_input *di    = (struct dinput_input*)data;

	if (port >= MAX_USERS)
		return 0;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         {
            settings                   = config_get_ptr();

            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            {
               unsigned i;
               int16_t ret = joypad->state(
                              joypad_info, binds[port], port);

               if (settings->uints.input_mouse_index[port] == 0)
               {
                  for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
                  {
                     if (binds[port][i].valid)
                     {
                        if (dinput_mouse_button_pressed(
                                 di, port, binds[port][i].mbutton)
                           )
                           ret |= (1 << i);
                     }
                  }
               }

               if (!input_dinput.keyboard_mapping_blocked)
               {
                  for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
                  {
                     if (binds[port][i].valid)
                     {
                        if ((binds[port][i].key < RETROK_LAST) &&
                              di->state[rarch_keysym_lut
                              [(enum retro_key)binds[port][i].key]] & 0x80)
                           ret |= (1 << i);
                     }
                  }
               }
               return ret;
            }
            else
            {
               if (id < RARCH_BIND_LIST_END)
               {
                  if (binds[port][id].valid)
                  {
                     if (button_is_pressed(
                              joypad,
                              joypad_info, binds[port], port, id))
                        return 1;
                     else if  (binds[port][id].key < RETROK_LAST
                           && (di->state[rarch_keysym_lut
                              [(enum retro_key)binds[port][id].key]] & 0x80)
                           && (   (id == RARCH_GAME_FOCUS_TOGGLE) 
                              || !input_dinput.keyboard_mapping_blocked)
                           )
                        return 1;
                     else if (
                           settings->uints.input_mouse_index[port] == 0
                           && dinput_mouse_button_pressed(
                              di, port, binds[port][id].mbutton)
                           )
                        return 1;
                  }
               }
            }
         }
         break;
      case RETRO_DEVICE_KEYBOARD:
         return (id < RETROK_LAST) && 
            di->state[rarch_keysym_lut[(enum retro_key)id]] & 0x80;
      case RETRO_DEVICE_ANALOG:
         if (binds[port])
            return dinput_pressed_analog(di, binds[port], idx, id);
         break;
      case RETRO_DEVICE_MOUSE:
         {
            settings                   = config_get_ptr();
            if (settings->uints.input_mouse_index[port] == 0)
               return dinput_mouse_state(di, port, id);
         }
         break;
      case RARCH_DEVICE_MOUSE_SCREEN:
         {
            settings                   = config_get_ptr();
            if (settings->uints.input_mouse_index[ port ] == 0)
               return dinput_mouse_state_screen(di, port, id);
         }
         break;
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         return dinput_pointer_state(di, idx, id,
               device == RARCH_DEVICE_POINTER_SCREEN);

      case RETRO_DEVICE_LIGHTGUN:
         switch (id)
         {
            /*aiming*/
            case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
            case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
            case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
               return dinput_lightgun_aiming_state(di, idx, id);

               /*buttons*/
            case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
            case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_A:
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_B:
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_C:
            case RETRO_DEVICE_ID_LIGHTGUN_START:
            case RETRO_DEVICE_ID_LIGHTGUN_SELECT:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT:
            case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
               {
                  unsigned new_id = 0;
                  settings        = config_get_ptr();
                  switch (id)
                  {
                     case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
                        new_id = RARCH_LIGHTGUN_TRIGGER;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
                        new_id = RARCH_LIGHTGUN_RELOAD;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_AUX_A:
                        new_id = RARCH_LIGHTGUN_AUX_A;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_AUX_B:
                        new_id = RARCH_LIGHTGUN_AUX_B;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_AUX_C:
                        new_id = RARCH_LIGHTGUN_AUX_C;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_START:
                        new_id = RARCH_LIGHTGUN_START;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_SELECT:
                        new_id = RARCH_LIGHTGUN_SELECT;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP:
                        new_id = RARCH_LIGHTGUN_DPAD_UP;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN:
                        new_id = RARCH_LIGHTGUN_DPAD_DOWN;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT:
                        new_id = RARCH_LIGHTGUN_DPAD_LEFT;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT:
                        new_id = RARCH_LIGHTGUN_DPAD_RIGHT;
                        break;
                     case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
                        new_id = RARCH_LIGHTGUN_START;
                        break;
                  }
                  if (binds[port][new_id].valid)
                  {
                     if (button_is_pressed(joypad,
                              joypad_info,
                              binds[port], port, new_id))
                        return 1;
                     else if (
                               binds[port][new_id].key < RETROK_LAST
                           && !input_dinput.keyboard_mapping_blocked
                           && di->state[rarch_keysym_lut
                           [(enum retro_key)binds[port][new_id].key]] & 0x80
                           )
                        return 1;
                     else if (
                           settings->uints.input_mouse_index[port] == 0
                           && dinput_mouse_button_pressed(
                              di, port, binds[port][new_id].mbutton)
                           )
                        return 1;
                  }
               }
               break;
               /*deprecated*/
            case RETRO_DEVICE_ID_LIGHTGUN_X:
               return di->mouse_rel_x;
            case RETRO_DEVICE_ID_LIGHTGUN_Y:
               return di->mouse_rel_y;
         }
			break;
   }

   return 0;
}

/* These are defined in later SDKs, thus ifdeffed. */

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE                0x0245
#endif

#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN                  0x0246
#endif

#ifndef WM_POINTERUP
#define WM_POINTERUP                    0x0247
#endif

#ifndef GET_POINTERID_WPARAM
#define GET_POINTERID_WPARAM(wParam)   (LOWORD(wParam))
#endif

/* Stores x/y in client coordinates. */
static void dinput_pointer_store_pos(
      struct pointer_status *pointer, WPARAM lParam)
{
   POINT point;

   point.x = GET_X_LPARAM(lParam);
   point.y = GET_Y_LPARAM(lParam);
   ScreenToClient((HWND)video_driver_window_get(), &point);
   pointer->pointer_x = point.x;
   pointer->pointer_y = point.y;
}

static void dinput_add_pointer(struct dinput_input *di,
      struct pointer_status *new_pointer)
{
   struct pointer_status *insert_pos = NULL;

   new_pointer->next                 = NULL;
   insert_pos                        = &di->pointer_head;

   while (insert_pos->next)
      insert_pos                     = insert_pos->next;
   insert_pos->next                  = new_pointer;
}

static void dinput_delete_pointer(struct dinput_input *di, int pointer_id)
{
   struct pointer_status *check_pos  = &di->pointer_head;

   while (check_pos && check_pos->next)
   {
      if (check_pos->next->pointer_id == pointer_id)
      {
         struct pointer_status *to_delete = check_pos->next;
         check_pos->next                  = check_pos->next->next;
         free(to_delete);
      }
      check_pos = check_pos->next;
   }
}

static struct pointer_status *dinput_find_pointer(
      struct dinput_input *di, int pointer_id)
{
   struct pointer_status *check_pos = di->pointer_head.next;

   while (check_pos)
   {
      if (check_pos->pointer_id == pointer_id)
         break;
      check_pos = check_pos->next;
   }

   return check_pos;
}

static void dinput_clear_pointers(struct dinput_input *di)
{
   struct pointer_status *pointer = &di->pointer_head;

   while (pointer->next)
   {
      struct pointer_status *del = pointer->next;

      pointer->next = pointer->next->next;
      free(del);
   }
}

bool dinput_handle_message(void *data,
      UINT message, WPARAM wParam, LPARAM lParam)
{
   struct dinput_input *di = (struct dinput_input *)data;
   /* WM_POINTERDOWN   : Arrives for each new touch event
    *                    with a new ID - add to list.
    * WM_POINTERUP     : Arrives once the pointer is no
    *                    longer down - remove from list.
    * WM_POINTERUPDATE : arrives for both pressed and
    *                    hovering pointers - ignore hovering
   */

   switch (message)
   {
      case WM_NCLBUTTONDBLCLK:
         di->doubleclick_on_titlebar = true;
         break;
      case WM_MOUSEMOVE:
         di->window_pos_x = GET_X_LPARAM(lParam);
         di->window_pos_y = GET_Y_LPARAM(lParam);
         break;
      case WM_POINTERDOWN:
         {
            struct pointer_status *new_pointer =
               (struct pointer_status *)malloc(sizeof(struct pointer_status));

            if (!new_pointer)
            {
               RARCH_ERR("[DINPUT]: dinput_handle_message: pointer allocation in WM_POINTERDOWN failed.\n");
               return false;
            }

            new_pointer->pointer_id = GET_POINTERID_WPARAM(wParam);
            dinput_pointer_store_pos(new_pointer, lParam);
            dinput_add_pointer(di, new_pointer);
            return true;
         }
      case WM_POINTERUP:
         {
            int pointer_id = GET_POINTERID_WPARAM(wParam);
            dinput_delete_pointer(di, pointer_id);
            return true;
         }
      case WM_POINTERUPDATE:
         {
            int pointer_id                 = GET_POINTERID_WPARAM(wParam);
            struct pointer_status *pointer = dinput_find_pointer(di, pointer_id);
            if (pointer)
               dinput_pointer_store_pos(pointer, lParam);
            return true;
         }
      case WM_DEVICECHANGE:
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0500 /* 2K */
         if (  wParam == DBT_DEVICEARRIVAL  || 
               wParam == DBT_DEVICEREMOVECOMPLETE)
         {
            PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
            if(pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            {
#if 0
               PDEV_BROADCAST_DEVICEINTERFACE pDevInf = 
                  (PDEV_BROADCAST_DEVICEINTERFACE)pHdr;
#endif

               /* TODO/FIXME: Don't destroy everything, let's just 
                * handle new devices gracefully */
               joypad_driver_reinit(di, di->joypad_driver_name);
            }
         }
#endif
         break;
      case WM_MOUSEWHEEL:
         if (((short) HIWORD(wParam))/120 > 0)
            di->mouse_wu = true;
         if (((short) HIWORD(wParam))/120 < 0)
            di->mouse_wd = true;
         break;
      case WM_MOUSEHWHEEL:
         if (((short) HIWORD(wParam))/120 > 0)
            di->mouse_hwu = true;
         if (((short) HIWORD(wParam))/120 < 0)
            di->mouse_hwd = true;
         break;
   }

   return false;
}

static void dinput_free(void *data)
{
   struct dinput_input *di = (struct dinput_input*)data;
   LPDIRECTINPUT8 hold_ctx = g_dinput_ctx;

   if (!di)
      return;

   /* Prevent a joypad driver to kill our context prematurely. */
   g_dinput_ctx = NULL;

#ifndef _XBOX
   win32_unset_input_userdata();
#endif

   g_dinput_ctx = hold_ctx;

   /* Clear any leftover pointers. */
   dinput_clear_pointers(di);

   if (di->keyboard)
      IDirectInputDevice8_Release(di->keyboard);

   if (di->mouse)
      IDirectInputDevice8_Release(di->mouse);

   if (di->joypad_driver_name)
      free(di->joypad_driver_name);
   di->joypad_driver_name = NULL;

   free(di);

   dinput_destroy_context();
}

static void dinput_grab_mouse(void *data, bool state)
{
   struct dinput_input *di = (struct dinput_input*)data;

   IDirectInputDevice8_Unacquire(di->mouse);
   IDirectInputDevice8_SetCooperativeLevel(di->mouse,
      (HWND)video_driver_window_get(),
      state ?
      (DISCL_EXCLUSIVE    | DISCL_FOREGROUND) :
      (DISCL_NONEXCLUSIVE | DISCL_FOREGROUND));
   IDirectInputDevice8_Acquire(di->mouse);
}

static bool dinput_set_rumble(
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   if (joypad)
      return input_joypad_set_rumble(joypad, port, effect, strength);
   return false;
}

static uint64_t dinput_get_capabilities(void *data)
{
   uint64_t caps = 0;

   caps |= (1 << RETRO_DEVICE_JOYPAD);
   caps |= (1 << RETRO_DEVICE_MOUSE);
   caps |= (1 << RETRO_DEVICE_KEYBOARD);
   caps |= (1 << RETRO_DEVICE_LIGHTGUN);
   caps |= (1 << RETRO_DEVICE_POINTER);
   caps |= (1 << RETRO_DEVICE_ANALOG);

   return caps;
}

input_driver_t input_dinput = {
   dinput_init,
   dinput_poll,
   dinput_input_state,
   dinput_free,
   NULL,
   NULL,
   dinput_get_capabilities,
   "dinput",
   dinput_grab_mouse,
   NULL,
   dinput_set_rumble,
   false
};
