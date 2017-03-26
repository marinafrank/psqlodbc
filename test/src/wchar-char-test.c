#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>

#include "common.h"

// #define UNICODE
#ifndef	_T
#ifdef	UNICODE
#define _T(t)	L##t
#else
#define _T(t)	t
#endif /* UNICODE */
#endif /* _T */

#ifdef	WIN32
#define	stricmp	_stricmp
#else
#define stricmp strcasecmp
#endif

#include	"wchar-char-test-sjis.c"
#include	"wchar-char-test-utf8.c"

enum {
	SJIS_TEST
	,UTF8_TEST
};

int main(int argc, char **argv)
{
	int rc, testsw = -1;
	HSTMT hstmt = SQL_NULL_HSTMT;
	const char	*loc, *ptr;

	struct {
		const char *name;
		int id;
	} loctbl[] = {
		{ "sjis", SJIS_TEST }
		,{ "shiftjis", SJIS_TEST }
		,{ "932", SJIS_TEST }
		,{ "utf-8", UTF8_TEST }
		,{ "utf8", UTF8_TEST }
	};

	loc = setlocale(LC_ALL, "");
	if (NULL != (ptr = strchr(loc, '.')))
	{
		int i;

		ptr++;
		for (i = 0; i < sizeof(loctbl) / sizeof(loctbl[0]); i++)
		{
			if (stricmp(ptr, loctbl[i].name) == 0)
			{
				testsw = loctbl[i].id;
				break;
			}
		}
		if (testsw < 0)
		{
			printf("can't handle this locale\n");
			exit(1);
		}
	}
	test_connect_ext("");

	rc = SQLAllocHandle(SQL_HANDLE_STMT, conn, &hstmt);
	if (!SQL_SUCCEEDED(rc))
	{
		print_diag("failed to allocate stmt handle", SQL_HANDLE_DBC, conn);
		exit(1);
	}

	switch (testsw)
	{
		case SJIS_TEST:
			fprintf(stderr, "SJIS test\n");
			rc = sjis_test(hstmt);
			break;
		case UTF8_TEST:
			fprintf(stderr, "UTF8 test\n");
			rc = utf8_test(hstmt);
			break;
	}

	/* Clean up */
	test_disconnect();

	return 0;
}
