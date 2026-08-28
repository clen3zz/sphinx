// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/protocol_types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/* #line 72 "sphinxd/src/protocol.rl" */

namespace sphinx {

/* #line 20 "sphinxd/include/sphinx/protocol.h" */
static const signed char _actions[] = {0, 1, 0, 1,  1, 1,  3, 1,  4, 1,  5,  1,  6, 1,  7, 1,  8,
                                       1, 9, 1, 10, 1, 11, 1, 12, 1, 13, 1,  14, 1, 15, 1, 16, 2,
                                       0, 1, 2, 1,  0, 2,  1, 4,  2, 1,  12, 2,  2, 3,  2, 4,  5,
                                       2, 5, 6, 3,  1, 4,  5, 3,  4, 5,  6,  4,  1, 4,  5, 6,  0};

static const short _key_offsets[] = {
    0,   0,   7,   8,   9,   12,  16,  22,  24,  29,  31,  36,  38,  44,  45,  46,  48,  56,  64,
    66,  71,  73,  79,  82,  86,  94,  102, 104, 110, 113, 117, 125, 133, 136, 144, 147, 151, 152,
    154, 155, 158, 162, 168, 170, 173, 174, 182, 190, 193, 194, 195, 196, 199, 203, 207, 208, 209,
    210, 213, 217, 223, 227, 233, 239, 243, 249, 250, 251, 252, 255, 259, 265, 267, 270, 271, 279,
    287, 290, 291, 292, 293, 294, 295, 296, 299, 303, 309, 311, 316, 318, 323, 325, 331, 332, 333,
    335, 343, 351, 353, 358, 360, 366, 369, 373, 381, 389, 391, 397, 400, 404, 412, 420, 423, 431,
    434, 438, 440, 441, 444, 448, 454, 456, 461, 463, 468, 470, 476, 477, 478, 480, 488, 496, 498,
    503, 505, 511, 514, 518, 526, 534, 536, 542, 545, 549, 557, 565, 568, 576, 579, 583, 584, 585,
    586, 587, 588, 589, 590, 591, 592, 593, 594, 595, 596, 0};

static const char _trans_keys[] = {
    97,  100, 103, 105, 114, 115, 118, 100, 100, 32, 9,  13, 13, 32, 9,  10, 13, 32,  9,   10,  11,
    12,  48,  57,  32,  9,   13,  48,  57,  48,  57, 32, 9,  13, 48, 57, 48, 57, 13,  32,  9,   12,
    48,  57,  13,  10,  10,  13,  13,  32,  9,   10, 11, 12, 48, 57, 13, 32, 9,  10,  11,  12,  48,
    57,  48,  57,  32,  9,   13,  48,  57,  48,  57, 13, 32, 9,  12, 48, 57, 13, 48,  57,  10,  13,
    48,  57,  13,  32,  9,   10,  11,  12,  48,  57, 13, 32, 9,  10, 11, 12, 48, 57,  48,  57,  13,
    32,  9,   12,  48,  57,  13,  48,  57,  10,  13, 48, 57, 13, 32, 9,  10, 11, 12,  48,  57,  13,
    32,  9,   10,  11,  12,  48,  57,  13,  48,  57, 13, 32, 9,  10, 11, 12, 48, 57,  10,  48,  57,
    10,  13,  48,  57,  101, 99,  108, 114, 32,  9,  13, 13, 32, 9,  10, 13, 32, 9,   10,  11,  12,
    48,  57,  13,  48,  57,  10,  13,  32,  9,   10, 11, 12, 48, 57, 13, 32, 9,  10,  11,  12,  48,
    57,  10,  48,  57,  101, 116, 101, 32,  9,   13, 13, 32, 9,  10, 13, 32, 9,  10,  10,  101, 116,
    32,  9,   13,  13,  32,  9,   10,  13,  32,  9,  10, 11, 12, 13, 32, 9,  10, 13,  32,  9,   10,
    11,  12,  13,  32,  9,   10,  11,  12,  9,   10, 13, 32, 13, 32, 9,  10, 11, 12,  110, 99,  114,
    32,  9,   13,  13,  32,  9,   10,  13,  32,  9,  10, 11, 12, 48, 57, 13, 48, 57,  10,  13,  32,
    9,   10,  11,  12,  48,  57,  13,  32,  9,   10, 11, 12, 48, 57, 10, 48, 57, 101, 112, 108, 97,
    99,  101, 32,  9,   13,  13,  32,  9,   10,  13, 32, 9,  10, 11, 12, 48, 57, 32,  9,   13,  48,
    57,  48,  57,  32,  9,   13,  48,  57,  48,  57, 13, 32, 9,  12, 48, 57, 13, 10,  10,  13,  13,
    32,  9,   10,  11,  12,  48,  57,  13,  32,  9,  10, 11, 12, 48, 57, 48, 57, 32,  9,   13,  48,
    57,  48,  57,  13,  32,  9,   12,  48,  57,  13, 48, 57, 10, 13, 48, 57, 13, 32,  9,   10,  11,
    12,  48,  57,  13,  32,  9,   10,  11,  12,  48, 57, 48, 57, 13, 32, 9,  12, 48,  57,  13,  48,
    57,  10,  13,  48,  57,  13,  32,  9,   10,  11, 12, 48, 57, 13, 32, 9,  10, 11,  12,  48,  57,
    13,  48,  57,  13,  32,  9,   10,  11,  12,  48, 57, 10, 48, 57, 10, 13, 48, 57,  101, 116, 116,
    32,  9,   13,  13,  32,  9,   10,  13,  32,  9,  10, 11, 12, 48, 57, 32, 9,  13,  48,  57,  48,
    57,  32,  9,   13,  48,  57,  48,  57,  13,  32, 9,  12, 48, 57, 13, 10, 10, 13,  13,  32,  9,
    10,  11,  12,  48,  57,  13,  32,  9,   10,  11, 12, 48, 57, 48, 57, 32, 9,  13,  48,  57,  48,
    57,  13,  32,  9,   12,  48,  57,  13,  48,  57, 10, 13, 48, 57, 13, 32, 9,  10,  11,  12,  48,
    57,  13,  32,  9,   10,  11,  12,  48,  57,  48, 57, 13, 32, 9,  12, 48, 57, 13,  48,  57,  10,
    13,  48,  57,  13,  32,  9,   10,  11,  12,  48, 57, 13, 32, 9,  10, 11, 12, 48,  57,  13,  48,
    57,  13,  32,  9,   10,  11,  12,  48,  57,  10, 48, 57, 10, 13, 48, 57, 97, 116, 115, 13,  10,
    101, 114, 115, 105, 111, 110, 13,  10,  0};

static const signed char _single_lengths[] = {
    0, 7, 1, 1, 1, 2, 2, 0, 1, 0, 1, 0, 2, 1, 1, 2, 2, 2, 0, 1, 0, 2, 1, 2, 2, 2, 0, 2,
    1, 2, 2, 2, 1, 2, 1, 2, 1, 2, 1, 1, 2, 2, 0, 1, 1, 2, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1,
    1, 1, 2, 2, 2, 2, 2, 4, 2, 1, 1, 1, 1, 2, 2, 0, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 0, 1, 0, 1, 0, 2, 1, 1, 2, 2, 2, 0, 1, 0, 2, 1, 2, 2, 2, 0, 2, 1, 2, 2, 2, 1,
    2, 1, 2, 2, 1, 1, 2, 2, 0, 1, 0, 1, 0, 2, 1, 1, 2, 2, 2, 0, 1, 0, 2, 1, 2, 2, 2, 0,
    2, 1, 2, 2, 2, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0};

static const signed char _range_lengths[] = {
    0, 0, 0, 0, 1, 1, 2, 1, 2, 1, 2, 1, 2, 0, 0, 0, 3, 3, 1, 2, 1, 2, 1, 1, 3, 3, 1, 2,
    1, 1, 3, 3, 1, 3, 1, 1, 0, 0, 0, 1, 1, 2, 1, 1, 0, 3, 3, 1, 0, 0, 0, 1, 1, 1, 0, 0,
    0, 1, 1, 2, 1, 2, 2, 0, 2, 0, 0, 0, 1, 1, 2, 1, 1, 0, 3, 3, 1, 0, 0, 0, 0, 0, 0, 1,
    1, 2, 1, 2, 1, 2, 1, 2, 0, 0, 0, 3, 3, 1, 2, 1, 2, 1, 1, 3, 3, 1, 2, 1, 1, 3, 3, 1,
    3, 1, 1, 0, 0, 1, 1, 2, 1, 2, 1, 2, 1, 2, 0, 0, 0, 3, 3, 1, 2, 1, 2, 1, 1, 3, 3, 1,
    2, 1, 1, 3, 3, 1, 3, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static const short _index_offsets[] = {
    0,   0,   8,   10,  12,  15,  19,  24,  26,  30,  32,  36,  38,  43,  45,  47,  50,  56,  62,
    64,  68,  70,  75,  78,  82,  88,  94,  96,  101, 104, 108, 114, 120, 123, 129, 132, 136, 138,
    141, 143, 146, 150, 155, 157, 160, 162, 168, 174, 177, 179, 181, 183, 186, 190, 194, 196, 198,
    200, 203, 207, 212, 216, 221, 226, 231, 236, 238, 240, 242, 245, 249, 254, 256, 259, 261, 267,
    273, 276, 278, 280, 282, 284, 286, 288, 291, 295, 300, 302, 306, 308, 312, 314, 319, 321, 323,
    326, 332, 338, 340, 344, 346, 351, 354, 358, 364, 370, 372, 377, 380, 384, 390, 396, 399, 405,
    408, 412, 415, 417, 420, 424, 429, 431, 435, 437, 441, 443, 448, 450, 452, 455, 461, 467, 469,
    473, 475, 480, 483, 487, 493, 499, 501, 506, 509, 513, 519, 525, 528, 534, 537, 541, 543, 545,
    547, 549, 551, 553, 555, 557, 559, 561, 563, 565, 567, 0};

static const short _cond_targs[] = {
    2,   36,  55,  65,  77,  115, 154, 0,   3,   0,   4,   0,   5,   5,   0,   0,   0,   0,   6,
    7,   7,   7,   16,  6,   8,   0,   9,   9,   8,   0,   10,  0,   11,  11,  10,  0,   12,  0,
    15,  13,  13,  12,  0,   14,  0,   162, 0,   162, 14,  0,   7,   7,   7,   16,  17,  6,   18,
    18,  18,  24,  17,  6,   19,  0,   20,  20,  19,  0,   21,  0,   23,  22,  22,  21,  0,   14,
    12,  0,   162, 14,  12,  0,   7,   7,   7,   16,  25,  6,   26,  26,  26,  30,  25,  6,   27,
    0,   29,  28,  28,  27,  0,   14,  21,  0,   162, 14,  21,  0,   7,   7,   7,   16,  31,  6,
    35,  32,  32,  33,  31,  6,   14,  27,  0,   34,  7,   7,   16,  31,  6,   162, 8,   0,   162,
    14,  27,  0,   37,  0,   38,  48,  0,   39,  0,   40,  40,  0,   0,   0,   0,   41,  42,  42,
    42,  45,  41,  43,  0,   44,  43,  0,   162, 0,   42,  42,  42,  45,  46,  41,  47,  42,  42,
    45,  46,  41,  162, 43,  0,   49,  0,   50,  0,   51,  0,   52,  52,  0,   0,   0,   0,   53,
    54,  0,   0,   53,  162, 0,   56,  0,   57,  0,   58,  58,  0,   0,   0,   0,   59,  63,  60,
    60,  64,  59,  0,   0,   0,   61,  63,  60,  60,  62,  61,  63,  60,  60,  62,  61,  0,   162,
    0,   0,   61,  63,  60,  60,  64,  59,  66,  0,   67,  0,   68,  0,   69,  69,  0,   0,   0,
    0,   70,  71,  71,  71,  74,  70,  72,  0,   73,  72,  0,   162, 0,   71,  71,  71,  74,  75,
    70,  76,  71,  71,  74,  75,  70,  162, 72,  0,   78,  0,   79,  0,   80,  0,   81,  0,   82,
    0,   83,  0,   84,  84,  0,   0,   0,   0,   85,  86,  86,  86,  95,  85,  87,  0,   88,  88,
    87,  0,   89,  0,   90,  90,  89,  0,   91,  0,   94,  92,  92,  91,  0,   93,  0,   162, 0,
    162, 93,  0,   86,  86,  86,  95,  96,  85,  97,  97,  97,  103, 96,  85,  98,  0,   99,  99,
    98,  0,   100, 0,   102, 101, 101, 100, 0,   93,  91,  0,   162, 93,  91,  0,   86,  86,  86,
    95,  104, 85,  105, 105, 105, 109, 104, 85,  106, 0,   108, 107, 107, 106, 0,   93,  100, 0,
    162, 93,  100, 0,   86,  86,  86,  95,  110, 85,  114, 111, 111, 112, 110, 85,  93,  106, 0,
    113, 86,  86,  95,  110, 85,  162, 87,  0,   162, 93,  106, 0,   116, 149, 0,   117, 0,   118,
    118, 0,   0,   0,   0,   119, 120, 120, 120, 129, 119, 121, 0,   122, 122, 121, 0,   123, 0,
    124, 124, 123, 0,   125, 0,   128, 126, 126, 125, 0,   127, 0,   162, 0,   162, 127, 0,   120,
    120, 120, 129, 130, 119, 131, 131, 131, 137, 130, 119, 132, 0,   133, 133, 132, 0,   134, 0,
    136, 135, 135, 134, 0,   127, 125, 0,   162, 127, 125, 0,   120, 120, 120, 129, 138, 119, 139,
    139, 139, 143, 138, 119, 140, 0,   142, 141, 141, 140, 0,   127, 134, 0,   162, 127, 134, 0,
    120, 120, 120, 129, 144, 119, 148, 145, 145, 146, 144, 119, 127, 140, 0,   147, 120, 120, 129,
    144, 119, 162, 121, 0,   162, 127, 140, 0,   150, 0,   151, 0,   152, 0,   153, 0,   162, 0,
    155, 0,   156, 0,   157, 0,   158, 0,   159, 0,   160, 0,   161, 0,   162, 0,   0,   0,   1,
    2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,
    97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
    135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 0};

static const signed char _cond_actions[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  3,  3,  3,  3,  0,
    45, 0,  7,  7,  5,  0,  45, 0,  9,  9,  5,  0,  45, 0,  11, 11, 11, 5,  0,  0,  0,  15, 0,  15,
    0,  0,  3,  3,  3,  3,  45, 0,  39, 39, 39, 39, 5,  0,  45, 0,  48, 48, 5,  0,  45, 0,  51, 51,
    51, 5,  0,  0,  45, 0,  15, 0,  45, 0,  3,  3,  3,  3,  45, 0,  54, 54, 54, 54, 5,  0,  45, 0,
    58, 58, 58, 5,  0,  0,  45, 0,  15, 0,  45, 0,  3,  3,  3,  3,  45, 0,  62, 62, 62, 62, 5,  0,
    0,  45, 0,  3,  3,  3,  3,  45, 0,  15, 45, 0,  15, 0,  45, 0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  1,  3,  3,  3,  3,  0,  45, 0,  23, 5,  0,  27, 0,  3,  3,  3,  3,  45, 0,
    42, 3,  3,  3,  5,  0,  27, 45, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  3,  0,
    0,  0,  21, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  3,  3,  3,  3,  0,  0,  0,  0,  1,
    3,  3,  3,  3,  0,  3,  3,  3,  33, 1,  0,  19, 0,  0,  1,  3,  3,  3,  36, 1,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  1,  3,  3,  3,  3,  0,  45, 0,  23, 5,  0,  25, 0,  3,  3,  3,
    3,  45, 0,  42, 3,  3,  3,  5,  0,  25, 45, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  1,  3,  3,  3,  3,  0,  45, 0,  7,  7,  5,  0,  45, 0,  9,  9,  5,  0,
    45, 0,  11, 11, 11, 5,  0,  0,  0,  17, 0,  17, 0,  0,  3,  3,  3,  3,  45, 0,  39, 39, 39, 39,
    5,  0,  45, 0,  48, 48, 5,  0,  45, 0,  51, 51, 51, 5,  0,  0,  45, 0,  17, 0,  45, 0,  3,  3,
    3,  3,  45, 0,  54, 54, 54, 54, 5,  0,  45, 0,  58, 58, 58, 5,  0,  0,  45, 0,  17, 0,  45, 0,
    3,  3,  3,  3,  45, 0,  62, 62, 62, 62, 5,  0,  0,  45, 0,  3,  3,  3,  3,  45, 0,  17, 45, 0,
    17, 0,  45, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  3,  3,  3,  3,  0,  45, 0,  7,
    7,  5,  0,  45, 0,  9,  9,  5,  0,  45, 0,  11, 11, 11, 5,  0,  0,  0,  13, 0,  13, 0,  0,  3,
    3,  3,  3,  45, 0,  39, 39, 39, 39, 5,  0,  45, 0,  48, 48, 5,  0,  45, 0,  51, 51, 51, 5,  0,
    0,  45, 0,  13, 0,  45, 0,  3,  3,  3,  3,  45, 0,  54, 54, 54, 54, 5,  0,  45, 0,  58, 58, 58,
    5,  0,  0,  45, 0,  13, 0,  45, 0,  3,  3,  3,  3,  45, 0,  62, 62, 62, 62, 5,  0,  0,  45, 0,
    3,  3,  3,  3,  45, 0,  13, 45, 0,  13, 0,  45, 0,  0,  0,  0,  0,  0,  0,  0,  0,  31, 0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  29, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

static const int start = 1;
static const int error = 0;

static const int en_main = 1;

/* #line 76 "sphinxd/src/protocol.rl" */

enum class Opcode {
  Set,
  Add,
  Replace,
  Get,
  Delete,
  Incr,
  Decr,
  Version,
  Stats,
};

class Parser {
  int _fsm_cs;
  std::optional<ParsedCommand> _command;
  ParseStatus _status = ParseStatus::Incomplete;
  std::optional<Opcode> opcode_;
  const char* key_start_ = nullptr;
  std::vector<std::string> keys_;
  uint64_t number_ = 0;
  bool number_overflow_ = false;
  bool number_token_overflow_ = false;
  uint64_t flags_ = 0;
  uint64_t expiration_ = 0;
  uint64_t body_size_ = 0;
  uint64_t delta_ = 0;

 public:
  const std::optional<ParsedCommand>& command() const noexcept {
    return _command;
  }

  ParseStatus status() const noexcept {
    return _status;
  }

  bool number_overflow() const noexcept {
    return number_overflow_;
  }

  Parser() {
    /* #line 451 "sphinxd/include/sphinx/protocol.h" */
    {
      _fsm_cs = (int)start;
    }

    /* #line 125 "sphinxd/src/protocol.rl" */
  }

  size_t parse(std::string_view msg) {
    _fsm_cs = start;
    _command.reset();
    _status = ParseStatus::Incomplete;
    opcode_.reset();
    key_start_ = nullptr;
    keys_.clear();
    number_ = 0;
    number_overflow_ = false;
    number_token_overflow_ = false;
    flags_ = 0;
    expiration_ = 0;
    body_size_ = 0;
    delta_ = 0;
    if (msg.empty()) {
      return 0;
    }
    auto* input_start = msg.data();
    auto header_end = msg.find("\r\n");
    if (header_end == std::string_view::npos) {
      // 命令头要等到 CRLF 到达才算完整。保持命令为空可通知 reactor 保留接收缓冲区。
      return 0;
    }
    auto* parse_end = input_start + header_end + 2;
    parse(input_start, parse_end);
    // 解析器刻意限制在第一个 CRLF 之前，因此即使当前行完整但无效，也只消费该行，
    // 不会吞掉后续的流水线命令。
    const auto header_size = header_end + 2;
    if (opcode_) {
      build_command(header_size);
      _status = ParseStatus::Parsed;
    } else {
      _status = ParseStatus::Invalid;
    }
    return header_size;
  }

 private:
  void build_command(size_t header_size) {
    const auto key_value = keys_.empty()
                               ? std::string_view{}
                               : std::string_view{keys_.front().data(), keys_.front().size()};
    const StorageBody body{body_size_, header_size};
    switch (opcode_.value()) {
      case Opcode::Set:
        _command = ParsedCommand{SetCommand{std::string{key_value}, flags_, expiration_, body}};
        break;
      case Opcode::Add:
        _command = ParsedCommand{AddCommand{std::string{key_value}, flags_, expiration_, body}};
        break;
      case Opcode::Replace:
        _command = ParsedCommand{ReplaceCommand{std::string{key_value}, flags_, expiration_, body}};
        break;
      case Opcode::Get:
        _command = ParsedCommand{GetCommand{std::move(keys_)}};
        break;
      case Opcode::Delete:
        _command = ParsedCommand{DeleteCommand{std::string{key_value}}};
        break;
      case Opcode::Incr:
        _command = ParsedCommand{IncrCommand{std::string{key_value}, delta_}};
        break;
      case Opcode::Decr:
        _command = ParsedCommand{DecrCommand{std::string{key_value}, delta_}};
        break;
      case Opcode::Version:
        _command = ParsedCommand{VersionCommand{}};
        break;
      case Opcode::Stats:
        _command = ParsedCommand{StatsCommand{}};
        break;
    }
    keys_.clear();
  }

  const char* parse(const char* p, const char* pe) {
    /* #line 537 "sphinxd/include/sphinx/protocol.h" */
    {
      int _klen;
      unsigned int _trans = 0;
      const char* _keys;
      const signed char* _acts;
      unsigned int _nacts;
    _resume: {}
      if (p == pe) goto _out;
      _keys = (_trans_keys + (_key_offsets[_fsm_cs]));
      _trans = (unsigned int)_index_offsets[_fsm_cs];

      _klen = (int)_single_lengths[_fsm_cs];
      if (_klen > 0) {
        const char* _lower = _keys;
        const char* _upper = _keys + _klen - 1;
        const char* _mid;
        while (1) {
          if (_upper < _lower) {
            _keys += _klen;
            _trans += (unsigned int)_klen;
            break;
          }

          _mid = _lower + ((_upper - _lower) >> 1);
          if (((*(p))) < (*(_mid)))
            _upper = _mid - 1;
          else if (((*(p))) > (*(_mid)))
            _lower = _mid + 1;
          else {
            _trans += (unsigned int)(_mid - _keys);
            goto _match;
          }
        }
      }

      _klen = (int)_range_lengths[_fsm_cs];
      if (_klen > 0) {
        const char* _lower = _keys;
        const char* _upper = _keys + (_klen << 1) - 2;
        const char* _mid;
        while (1) {
          if (_upper < _lower) {
            _trans += (unsigned int)_klen;
            break;
          }

          _mid = _lower + (((_upper - _lower) >> 1) & ~1);
          if (((*(p))) < (*(_mid)))
            _upper = _mid - 2;
          else if (((*(p))) > (*(_mid + 1)))
            _lower = _mid + 2;
          else {
            _trans += (unsigned int)((_mid - _keys) >> 1);
            break;
          }
        }
      }

    _match: {}
      _fsm_cs = (int)_cond_targs[_trans];

      if (_cond_actions[_trans] != 0) {
        _acts = (_actions + (_cond_actions[_trans]));
        _nacts = (unsigned int)(*(_acts));
        _acts += 1;
        while (_nacts > 0) {
          switch ((*(_acts))) {
            case 0: {
              {
                /* #line 20 "sphinxd/src/protocol.rl" */

                key_start_ = p;
              }

              /* #line 614 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 1: {
              {
                /* #line 24 "sphinxd/src/protocol.rl" */

                keys_.emplace_back(key_start_, static_cast<size_t>(p - key_start_));
              }

              /* #line 624 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 2: {
              {
                /* #line 32 "sphinxd/src/protocol.rl" */
                number_ = 0;
                number_token_overflow_ = false;
              }

              /* #line 632 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 3: {
              {
                /* #line 32 "sphinxd/src/protocol.rl" */

                if (!number_token_overflow_) {
                  auto digit_value = uint64_t((((*(p)))) - '0');
                  if (number_ > (std::numeric_limits<uint64_t>::max() - digit_value) / 10) {
                    number_token_overflow_ = true;
                    number_overflow_ = true;
                  } else {
                    number_ = number_ * 10 + digit_value;
                  }
                }
              }

              /* #line 650 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 4: {
              {
                /* #line 44 "sphinxd/src/protocol.rl" */
                flags_ = number_;
              }

              /* #line 658 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 5: {
              {
                /* #line 46 "sphinxd/src/protocol.rl" */
                expiration_ = number_;
              }

              /* #line 666 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 6: {
              {
                /* #line 48 "sphinxd/src/protocol.rl" */
                body_size_ = number_;
              }

              /* #line 674 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 7: {
              {
                /* #line 50 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Set;
              }

              /* #line 682 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 8: {
              {
                /* #line 52 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Add;
              }

              /* #line 690 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 9: {
              {
                /* #line 54 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Replace;
              }

              /* #line 698 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 10: {
              {
                /* #line 56 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Get;
              }

              /* #line 706 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 11: {
              {
                /* #line 58 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Delete;
              }

              /* #line 714 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 12: {
              {
                /* #line 60 "sphinxd/src/protocol.rl" */
                delta_ = number_;
              }

              /* #line 722 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 13: {
              {
                /* #line 62 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Incr;
              }

              /* #line 730 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 14: {
              {
                /* #line 64 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Decr;
              }

              /* #line 738 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 15: {
              {
                /* #line 66 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Version;
              }

              /* #line 746 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
            case 16: {
              {
                /* #line 68 "sphinxd/src/protocol.rl" */
                opcode_ = Opcode::Stats;
              }

              /* #line 754 "sphinxd/include/sphinx/protocol.h" */

              break;
            }
          }
          _nacts -= 1;
          _acts += 1;
        }
      }

      if (_fsm_cs != 0) {
        p += 1;
        goto _resume;
      }
    _out: {}
    }

    /* #line 207 "sphinxd/src/protocol.rl" */

    return p;
  }
};

}  // namespace sphinx
