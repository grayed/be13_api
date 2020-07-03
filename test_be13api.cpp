// Uses cester
// doc: https://github.com/exoticlibraries/libcester/blob/master/docs/docs/macros.rst

// cester generates some GCC warnings. Ignore them.

// define stuff I need in the global environment. Only read it once.
#ifndef GUARD_BLOCK
#include "config.h"

#ifdef DFXML_GNUC_HAS_IGNORED_SHADOW_PRAGMA
#pragma GCC diagnostic ignored "-Wshadow"
#endif

#ifdef DFXML_GNUC_HAS_IGNORED_UNUSED_VARIABLE_PRAGMA
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#ifdef DFXML_GNUC_HAS_IGNORED_UNUSED_LABEL_PRAGMA
#pragma GCC diagnostic ignored "-Wunused-label"
#endif

// cester wants unix to be defined on Apple
#ifdef __APPLE__
#define unix
#endif

// define stuff I need in the global environment. Only read it once.
//#include "../bulk_extractor_i.h"
#define GUARD_BLOCK
#endif

// get cester!
#include "cester.h"

CESTER_TEST(test_regex_1, inst, cester_assert_true(  regex_list::is_regex("this[1234]foo"));    )
CESTER_TEST(test_regex_1, inst, cester_assert_true(  regex_list::is_regex("this(1234)foo"));    )
CESTER_TEST(test_regex_1, inst, cester_assert_true(  regex_list::is_regex("this[1234].*foo"));    )
CESTER_TEST(test_regex_1, inst, cester_assert_false( regex_list::is_regex("this1234foo"));    )


