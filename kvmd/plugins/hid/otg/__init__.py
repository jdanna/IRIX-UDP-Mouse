# ========================================================================== #
#                                                                            #
#    KVMD - The main PiKVM daemon.                                           #
#                                                                            #
#    Copyright (C) 2018-2024  Maxim Devaev <mdevaev@gmail.com>               #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
# ========================================================================== #


import asyncio
import copy
import socket

from typing import AsyncGenerator
from typing import Any

from ....logging import get_logger

from .... import aiomulti
from .... import usb

from ....yamlconf import Option

from ....validators.basic import valid_bool
from ....validators.basic import valid_int_f1
from ....validators.basic import valid_float_f01
from ....validators.net import valid_ip_or_host
from ....validators.net import valid_port
from ....validators.os import valid_abs_path

from .. import BaseHid

from .keyboard import KeyboardProcess
from .mouse import MouseProcess


# Evdev button code → name mapping (matches kvmd's MOUSE_TO_EVDEV)
_BUTTON_CODES: dict[int, str] = {
    272: "left",
    273: "right",
    274: "middle",
    275: "up",
    276: "down",
}


# =====
class Plugin(BaseHid):  # pylint: disable=too-many-instance-attributes
    def __init__(
        self,
        ignore_keys: list[str],
        mouse_x_range: dict[str, Any],
        mouse_y_range: dict[str, Any],
        jiggler: dict[str, Any],

        keyboard: dict[str, Any],
        mouse: dict[str, Any],
        mouse_alt: dict[str, Any],
        noop: bool,

        irix_host: str,
        irix_port: int,
        irix_screen_width: int,
        irix_screen_height: int,

        udc: str,  # XXX: Not from options, see /kvmd/apps/kvmd/__init__.py for details
    ) -> None:

        super().__init__(ignore_keys=ignore_keys, **mouse_x_range, **mouse_y_range, **jiggler)

        self.__udc = udc

        self.__notifier = aiomulti.AioMpNotifier()

        win98_fix = mouse.pop("absolute_win98_fix")
        common = {"notifier": self.__notifier, "noop": noop}

        self.__keyboard_proc = KeyboardProcess(**common, **keyboard)
        self.__mouse_current = self.__mouse_proc = MouseProcess(**common, **mouse)

        self.__mouse_alt_proc: (MouseProcess | None) = None
        self.__mouses: dict[str, MouseProcess] = {}
        if mouse_alt["device_path"]:
            self.__mouse_alt_proc = MouseProcess(
                absolute=(not mouse["absolute"]),
                **common,
                **mouse_alt,
            )
            self.__mouses = {
                "usb": (self.__mouse_proc if mouse["absolute"] else self.__mouse_alt_proc),
                "usb_rel": (self.__mouse_alt_proc if mouse["absolute"] else self.__mouse_proc),
            }
            if win98_fix:
                # На самом деле мультимышка и win95 не зависят друг от друга,
                # но так было проще реализовать переключение режимов
                self.__mouses["usb_win98"] = self.__mouses["usb"]

        self._set_jiggler_absolute(self.__mouse_current.is_absolute())

        self.__irix_host = irix_host
        self.__irix_port = irix_port
        self.__irix_screen_width = irix_screen_width
        self.__irix_screen_height = irix_screen_height

    @classmethod
    def get_plugin_options(cls) -> dict:
        return {
            "keyboard": {
                "device":         Option("/dev/kvmd-hid-keyboard", type=valid_abs_path, unpack_as="device_path"),
                "select_timeout": Option(0.1, type=valid_float_f01),
                "queue_timeout":  Option(0.1, type=valid_float_f01),
                "write_retries":  Option(150, type=valid_int_f1),
            },
            "mouse": {
                "device":             Option("/dev/kvmd-hid-mouse", type=valid_abs_path, unpack_as="device_path"),
                "select_timeout":     Option(0.1,   type=valid_float_f01),
                "queue_timeout":      Option(0.1,   type=valid_float_f01),
                "write_retries":      Option(150,   type=valid_int_f1),
                "absolute":           Option(True,  type=valid_bool),
                "absolute_win98_fix": Option(False, type=valid_bool),
                "horizontal_wheel":   Option(True,  type=valid_bool),
            },
            "mouse_alt": {
                "device":           Option("/dev/kvmd-hid-mouse-alt", type=valid_abs_path, if_empty="", unpack_as="device_path"),
                "select_timeout":   Option(0.1,  type=valid_float_f01),
                "queue_timeout":    Option(0.1,  type=valid_float_f01),
                "write_retries":    Option(150,  type=valid_int_f1),
                # No absolute option here, initialized by (not mouse.absolute)
                # Also no absolute_win98_fix
                "horizontal_wheel": Option(True, type=valid_bool),
            },
            "noop": Option(False, type=valid_bool),
            "irix_host":          Option("",   type=valid_ip_or_host, if_empty=""),
            "irix_port":          Option(5005, type=valid_port),
            "irix_screen_width":  Option(1920, type=valid_int_f1),
            "irix_screen_height": Option(1200, type=valid_int_f1),
            **cls._get_base_options(),
        }

    async def sysprep(self) -> None:
        udc = usb.find_udc(self.__udc)
        get_logger(0).info("Using UDC %s", udc)
        self.__keyboard_proc.start(udc)
        self.__mouse_proc.start(udc)
        if self.__mouse_alt_proc:
            self.__mouse_alt_proc.start(udc)

    async def get_state(self) -> dict:
        keyboard_state = await self.__keyboard_proc.get_state()
        mouse_state = await self.__mouse_current.get_state()
        return {
            "enabled": True,
            "online": True,
            "busy": False,
            "connected": None,
            "keyboard": {
                "online": keyboard_state["online"],
                "leds": {
                    "caps": keyboard_state["caps"],
                    "scroll": keyboard_state["scroll"],
                    "num": keyboard_state["num"],
                },
                "outputs": {"available": [], "active": ""},
            },
            "mouse": {
                "outputs": {
                    "available": list(self.__mouses),
                    "active": self.__get_current_mouse_mode(),
                },
                **mouse_state,
            },
            "irix": {
                "host": self.__irix_host,
                "port": self.__irix_port,
                "screen_width": self.__irix_screen_width,
                "screen_height": self.__irix_screen_height,
            },
            **self._get_jiggler_state(),
        }

    async def trigger_state(self) -> None:
        self.__notifier.notify(1)

    async def poll_state(self) -> AsyncGenerator[dict, None]:
        prev: dict = {}
        while True:
            if (await self.__notifier.wait()) > 0:
                prev = {}
            new = await self.get_state()
            if new != prev:
                prev = copy.deepcopy(new)
                yield new

    async def reset(self) -> None:
        self.__keyboard_proc.send_reset_event()
        self.__mouse_proc.send_reset_event()
        if self.__mouse_alt_proc:
            self.__mouse_alt_proc.send_reset_event()

    async def cleanup(self) -> None:
        coros = [
            self.__keyboard_proc.cleanup(),
            self.__mouse_proc.cleanup(),
        ]
        if self.__mouse_alt_proc:
            coros.append(self.__mouse_alt_proc.cleanup())
        await asyncio.gather(*coros)

    # =====

    def set_params(
        self,
        keyboard_output: (str | None)=None,
        mouse_output: (str | None)=None,
        jiggler: (bool | None)=None,
        irix_host: (str | None)=None,
        irix_port: (int | None)=None,
        irix_screen_width: (int | None)=None,
        irix_screen_height: (int | None)=None,
    ) -> None:

        _ = keyboard_output
        if mouse_output in self.__mouses and mouse_output != self.__get_current_mouse_mode():
            self.__mouse_current.send_clear_event()
            self.__mouse_current = self.__mouses[mouse_output]
            self.__mouse_current.set_win98_fix(mouse_output == "usb_win98")
            self._set_jiggler_absolute(self.__mouse_current.is_absolute())
            self.__notifier.notify()
        if jiggler is not None:
            self._set_jiggler_active(jiggler)
            self.__notifier.notify()
        if irix_host is not None:
            self.__irix_host = irix_host
            self.__notifier.notify()
        if irix_port is not None:
            self.__irix_port = irix_port
            self.__notifier.notify()
        if irix_screen_width is not None:
            self.__irix_screen_width = irix_screen_width
            self.__notifier.notify()
        if irix_screen_height is not None:
            self.__irix_screen_height = irix_screen_height
            self.__notifier.notify()

    # =====

    def _send_mouse_button_event(self, button: int, state: bool) -> None:
        if self.__irix_host:
            button_name = _BUTTON_CODES.get(button)
            if button_name is None:
                get_logger(0).warning("IRIX: unknown mouse button code %d", button)
                return
            self.__udp_send(f"{button_name},{state}")
        else:
            self.__mouse_current.send_button_event(button, state)

    def _send_key_event(self, key: int, state: bool) -> None:
        self.__keyboard_proc.send_key_event(key, state)

    def _send_mouse_move_event(self, to_x: int, to_y: int) -> None:
        if self.__irix_host:
            input_min = -32768
            input_max = 32768
            pixel_x = int((to_x - input_min) / (input_max - input_min) * (self.__irix_screen_width - 1))
            pixel_y = int((to_y - input_min) / (input_max - input_min) * (self.__irix_screen_height - 1))
            self.__udp_send(f"{pixel_x}_{pixel_y}")
        else:
            self.__mouse_current.send_move_event(to_x, to_y)

    def _send_mouse_relative_event(self, delta_x: int, delta_y: int) -> None:
        self.__mouse_current.send_relative_event(delta_x, delta_y)

    def _send_mouse_wheel_event(self, delta_x: int, delta_y: int) -> None:
        if self.__irix_host:
            _ = delta_x
            self.__udp_send(f"WHEEL_{delta_y}")
        else:
            self.__mouse_current.send_wheel_event(delta_x, delta_y)

    def _clear_events(self) -> None:
        self.__keyboard_proc.send_clear_event()
        self.__mouse_proc.send_clear_event()
        if self.__mouse_alt_proc:
            self.__mouse_alt_proc.send_clear_event()

    # =====

    def __udp_send(self, message: str) -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.sendto(message.encode(), (self.__irix_host, self.__irix_port))
        except OSError as ex:
            get_logger(0).warning("IRIX: UDP send to %s:%d failed: %s", self.__irix_host, self.__irix_port, ex)

    def __get_current_mouse_mode(self) -> str:
        if len(self.__mouses) == 0:
            return ""
        if self.__mouse_current.is_absolute():
            return ("usb_win98" if self.__mouse_current.get_win98_fix() else "usb")
        return "usb_rel"
