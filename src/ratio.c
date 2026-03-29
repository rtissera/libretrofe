// SPDX-License-Identifier: MIT
#include "ratio.h"

static const libra_ratio_entry_t kRatios[] = {
    /* Standard aspect ratios (matching RetroArch's list) */
    { "4:3",    "4/3",          4.0f / 3.0f },
    { "16:9",   "16/9",         16.0f / 9.0f },
    { "16:10",  "16/10",        16.0f / 10.0f },
    { "16:15",  "16/15",        16.0f / 15.0f },
    { "21:9",   "21/9",         21.0f / 9.0f },
    { "1:1",    "1/1",          1.0f / 1.0f },
    { "2:1",    "2/1",          2.0f / 1.0f },
    { "3:2",    "3/2",          3.0f / 2.0f },
    { "3:4",    "3/4",          3.0f / 4.0f },
    { "4:1",    "4/1",          4.0f / 1.0f },
    { "9:16",   "9/16",         9.0f / 16.0f },
    { "5:4",    "5/4",          5.0f / 4.0f },
    { "6:5",    "6/5",          6.0f / 5.0f },
    { "7:9",    "7/9",          7.0f / 9.0f },
    { "8:3",    "8/3",          8.0f / 3.0f },
    { "8:7",    "8/7",          8.0f / 7.0f },
    { "19:12",  "19/12",        19.0f / 12.0f },
    { "19:14",  "19/14",        19.0f / 14.0f },
    { "30:17",  "30/17",        30.0f / 17.0f },
    { "32:9",   "32/9",         32.0f / 9.0f },
    /* Special modes */
    { "Config",        "config",       0.0f },
    { "Square pixel",  "squarepixel",  0.0f },
    { "Core provided", "core",         0.0f },
    { "Custom",        "custom",       0.0f },
    { "Full",          "full",         0.0f },
    /* Sentinel */
    { 0, 0, 0.0f }
};

const libra_ratio_entry_t *libra_get_ratios(void)
{
    return kRatios;
}

int libra_get_ratio_count(void)
{
    return (int)(sizeof(kRatios) / sizeof(kRatios[0])) - 1; /* exclude sentinel */
}
