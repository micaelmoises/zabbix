/*
** Zabbix
** Copyright (C) 2001-2017 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "zbxmocktest.h"
#include "zbxmockdata.h"
#include "zbxmockassert.h"

#include "common.h"

void cm_print_error(const char * const format, ...);

#define _FAIL(file, line, prefix, message, ...)							\
	do 											\
	{											\
		cm_print_error("%s%s" message, (NULL != prefix_msg ? prefix_msg : ""),		\
				(NULL != prefix_msg && '\0' != *prefix_msg ? ": " : ""),	\
				__VA_ARGS__);							\
		_fail(file, line);								\
	} while(0)

void	__zbx_mock_assert_streq(const char *file, int line, const char *prefix_msg, const char *expected_value,
		const char *return_value)
{
	if (0 != strcmp(return_value, expected_value))
		_FAIL(file, line, prefix_msg, "Expected \"%s\" while got \"%s\"\n", expected_value, return_value);
}

void	__zbx_mock_assert_strne(const char *file, int line, const char *prefix_msg, const char *expected_value,
		const char *return_value)
{
	if (0 == strcmp(return_value, expected_value))
		_FAIL(file, line, prefix_msg, "Expected not \"%s\" while got \"%s\"\n", expected_value, return_value);
}

void	__zbx_mock_assert_uint64eq(const char *file, int line, const char *prefix_msg, zbx_uint64_t expected_value,
		zbx_uint64_t return_value)
{
	if (return_value != expected_value)
	{
		_FAIL(file, line, prefix_msg, "Expected \"" ZBX_FS_UI64 "\" while got \"" ZBX_FS_UI64 "\"\n",
				expected_value, return_value);
	}
}

void	__zbx_mock_assert_uint64ne(const char *file, int line, const char *prefix_msg, zbx_uint64_t expected_value,
		zbx_uint64_t return_value)
{
	if (return_value == expected_value)
	{
		_FAIL(file, line, prefix_msg, "Expected not \"" ZBX_FS_UI64 "\" while got \"" ZBX_FS_UI64 "\"\n",
				expected_value, return_value);
	}
}

void	__zbx_mock_assert_inteq(const char *file, int line, const char *prefix_msg, int expected_value,
		int return_value)
{
	if (return_value != expected_value)
		_FAIL(file, line, prefix_msg, "Expected \"%d\" while got \"%d\"\n", expected_value, return_value);
}

void	__zbx_mock_assert_intne(const char *file, int line, const char *prefix_msg, int expected_value,
		int return_value)
{
	if (return_value == expected_value)
		_FAIL(file, line, prefix_msg, "Expected not \"%d\" while got \"%d\"\n", expected_value, return_value);
}
