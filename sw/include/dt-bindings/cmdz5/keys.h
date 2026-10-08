/*
 * Input codes for the CMD-Z5 keys that have no INPUT_KEY_* equivalent in
 * Zephyr. The values follow the Linux input codes where one exists.
 * Usable from devicetree and C.
 */

#pragma once

#define CMDZ5_KEY_CALL  169   /* Linux KEY_PHONE */
#define CMDZ5_KEY_MEMO  0x18c /* Linux KEY_MEMO */
#define CMDZ5_KEY_STAR  0x20a /* Linux KEY_NUMERIC_STAR */
#define CMDZ5_KEY_POUND 0x20b /* Linux KEY_NUMERIC_POUND */
/* Lid switch, reported as a key: value 1 = lid closed, 0 = open. */
#define CMDZ5_KEY_LID   0x2f0
