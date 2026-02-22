#pragma once

/*
 * SEMU Input Event Codes
 *
 * Input event type and code definitions used by the SEMU virtual input
 * subsystem. The numeric values are chosen to match the Linux evdev ABI so
 * that a guest kernel can consume them directly without translation.
 *
 * Reference: Linux kernel include/uapi/linux/input-event-codes.h
 *   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Event types
 *
 * Each input event carries a type that identifies the class of data it
 * reports: synchronization boundaries, key/button state changes, or
 * absolute axis positions.
 */

#define SEMU_EV_SYN 0x00
#define SEMU_EV_KEY 0x01
#define SEMU_EV_ABS 0x03
#define SEMU_EV_REP 0x14

/*
 * Synchronization codes
 *
 * A SYN_REPORT event marks the end of a coherent group of input events
 * (e.g. one key press, or one pointer movement with its axis values).
 */

#define SEMU_SYN_REPORT 0

/*
 * Keyboard scancodes
 *
 * Standard PC/AT keyboard codes covering the main block, function keys,
 * navigation cluster, and numeric keypad.
 */

/* Escape and number row */
#define SEMU_KEY_ESC 1
#define SEMU_KEY_1 2
#define SEMU_KEY_2 3
#define SEMU_KEY_3 4
#define SEMU_KEY_4 5
#define SEMU_KEY_5 6
#define SEMU_KEY_6 7
#define SEMU_KEY_7 8
#define SEMU_KEY_8 9
#define SEMU_KEY_9 10
#define SEMU_KEY_0 11
#define SEMU_KEY_MINUS 12
#define SEMU_KEY_EQUAL 13
#define SEMU_KEY_BACKSPACE 14

/* Top letter row */
#define SEMU_KEY_TAB 15
#define SEMU_KEY_Q 16
#define SEMU_KEY_W 17
#define SEMU_KEY_E 18
#define SEMU_KEY_R 19
#define SEMU_KEY_T 20
#define SEMU_KEY_Y 21
#define SEMU_KEY_U 22
#define SEMU_KEY_I 23
#define SEMU_KEY_O 24
#define SEMU_KEY_P 25
#define SEMU_KEY_LEFTBRACE 26
#define SEMU_KEY_RIGHTBRACE 27
#define SEMU_KEY_ENTER 28

/* Home row */
#define SEMU_KEY_LEFTCTRL 29
#define SEMU_KEY_A 30
#define SEMU_KEY_S 31
#define SEMU_KEY_D 32
#define SEMU_KEY_F 33
#define SEMU_KEY_G 34
#define SEMU_KEY_H 35
#define SEMU_KEY_J 36
#define SEMU_KEY_K 37
#define SEMU_KEY_L 38
#define SEMU_KEY_SEMICOLON 39
#define SEMU_KEY_APOSTROPHE 40
#define SEMU_KEY_GRAVE 41

/* Bottom letter row */
#define SEMU_KEY_LEFTSHIFT 42
#define SEMU_KEY_BACKSLASH 43
#define SEMU_KEY_Z 44
#define SEMU_KEY_X 45
#define SEMU_KEY_C 46
#define SEMU_KEY_V 47
#define SEMU_KEY_B 48
#define SEMU_KEY_N 49
#define SEMU_KEY_M 50
#define SEMU_KEY_COMMA 51
#define SEMU_KEY_DOT 52
#define SEMU_KEY_SLASH 53
#define SEMU_KEY_RIGHTSHIFT 54

/* Modifier and space row */
#define SEMU_KEY_LEFTALT 56
#define SEMU_KEY_SPACE 57
#define SEMU_KEY_CAPSLOCK 58

/* Function keys */
#define SEMU_KEY_F1 59
#define SEMU_KEY_F2 60
#define SEMU_KEY_F3 61
#define SEMU_KEY_F4 62
#define SEMU_KEY_F5 63
#define SEMU_KEY_F6 64
#define SEMU_KEY_F7 65
#define SEMU_KEY_F8 66
#define SEMU_KEY_F9 67
#define SEMU_KEY_F10 68
#define SEMU_KEY_F11 87
#define SEMU_KEY_F12 88

/* Numeric keypad */
#define SEMU_KEY_NUMLOCK 69
#define SEMU_KEY_SCROLLLOCK 70
#define SEMU_KEY_KP7 71
#define SEMU_KEY_KP8 72
#define SEMU_KEY_KP9 73
#define SEMU_KEY_KPASTERISK 55
#define SEMU_KEY_KPMINUS 74
#define SEMU_KEY_KP4 75
#define SEMU_KEY_KP5 76
#define SEMU_KEY_KP6 77
#define SEMU_KEY_KPPLUS 78
#define SEMU_KEY_KP1 79
#define SEMU_KEY_KP2 80
#define SEMU_KEY_KP3 81
#define SEMU_KEY_KP0 82
#define SEMU_KEY_KPDOT 83
#define SEMU_KEY_KPENTER 96
#define SEMU_KEY_KPSLASH 98

/* Right-side modifiers */
#define SEMU_KEY_RIGHTCTRL 97
#define SEMU_KEY_RIGHTALT 100

/* Navigation cluster */
#define SEMU_KEY_HOME 102
#define SEMU_KEY_UP 103
#define SEMU_KEY_PAGEUP 104
#define SEMU_KEY_LEFT 105
#define SEMU_KEY_RIGHT 106
#define SEMU_KEY_END 107
#define SEMU_KEY_DOWN 108
#define SEMU_KEY_PAGEDOWN 109
#define SEMU_KEY_INSERT 110
#define SEMU_KEY_DELETE 111

/*
 * Mouse button codes
 */
#define SEMU_BTN_LEFT 0x110
#define SEMU_BTN_RIGHT 0x111
#define SEMU_BTN_MIDDLE 0x112

/*
 * Absolute axis identifiers (used for pointer position reporting)
 */
#define SEMU_ABS_X 0x00
#define SEMU_ABS_Y 0x01

/*
 * Key-repeat configuration codes
 */
#define SEMU_REP_DELAY 0x00
#define SEMU_REP_PERIOD 0x01

/*
 * Device property flags (reported via VIRTIO_INPUT_CFG_PROP_BITS)
 */
#define SEMU_INPUT_PROP_POINTER 0x00
#define SEMU_INPUT_PROP_DIRECT 0x01
