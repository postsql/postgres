/*-------------------------------------------------------------------------
 *
 * Command line option processing facilities for frontend code
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/option_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/option_utils.h"

/*
 * Provide strictly harmonized handling of --help and --version
 * options.
 */
void
handle_help_version_opts(int argc, char *argv[],
						 const char *fixed_progname, help_handler hlp)
{
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			hlp(get_progname(argv[0]));
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			printf("%s (PostgreSQL) " PG_VERSION "\n", fixed_progname);
			exit(0);
		}
	}
}

/*
 * option_parse_int
 *
 * Parse integer value for an option.  If the parsing is successful, returns
 * true and stores the result in *result if that's given; if parsing fails,
 * returns false.
 */
bool
option_parse_int(const char *optarg, const char *optname,
				 int min_range, int max_range,
				 int *result)
{
	char	   *endptr;
	int			val;

	errno = 0;
	val = strtoint(optarg, &endptr, 10);

	/*
	 * Skip any trailing whitespace; if anything but whitespace remains before
	 * the terminating character, fail.
	 */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	if (*endptr != '\0')
	{
		pg_log_error("invalid value \"%s\" for option %s",
					 optarg, optname);
		return false;
	}

	if (errno == ERANGE || val < min_range || val > max_range)
	{
		pg_log_error("%s must be in range %d..%d",
					 optname, min_range, max_range);
		return false;
	}

	if (result)
		*result = val;
	return true;
}

/*
 * option_parse_uint32
 *
 * Parse unsigned integer value for an option.  If the parsing is successful,
 * returns true and stores the result in *result if that's given;
 * if parsing fails, returns false.
 */
bool
option_parse_uint32(const char *optarg, const char *optname,
				 uint32 min_range, uint32 max_range,
				 uint32 *result)
{
	char	   		*endptr;
	unsigned long	val;

	/* Fail if there is a minus sign at the start of value */
	while(isspace((unsigned char) *optarg))
		optarg++;
	if(*optarg == '-')
	{
		pg_log_error("value \"%s\" for option %s can not be negative",
					optarg, optname);
		return false;
	}

	errno = 0;
	val = strtoul(optarg, &endptr, 10);

	/*
	 * Skip any trailing whitespace; if anything but whitespace remains before
	 * the terminating character, fail.
	 */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	if (endptr == optarg || *endptr != '\0')
	{
		pg_log_error("invalid value \"%s\" for option %s",
					 optarg, optname);
		return false;
	}

	/* as min_range and max_range are uint32 then the range check will
	 * catch the case where unsigned long val is outside 32 bit range */
	if (errno == ERANGE || val < min_range || val > max_range)
	{
		pg_log_error("%s not in range %u..%u", optname, min_range, max_range);
		return false;
	}

	if (result)
		*result = (uint32) val;
	return true;
}

/*
 * option_parse_size_bytes
 *
 * Parse a positive integer size value with an optional unit suffix
 * (k/kB, M/MB, G/GB, T/TB, case-insensitive; default unit is M) into bytes.
 * Returns true and stores the byte count in *result on success; logs an error
 * and returns false on failure.
 */
bool
option_parse_size_bytes(const char *optarg, const char *optname,
						uint64 *result)
{
	const char *p = optarg;
	const char *unit_start;
	size_t		unit_len;
	uint64		val = 0;
	uint64		multiplier = 0;

	if (optarg == NULL)
	{
		pg_log_error("invalid value for %s: \"\"", optname);
		return false;
	}

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	if (!isdigit((unsigned char) *p))
	{
		pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
		return false;
	}

	while (isdigit((unsigned char) *p))
	{
		uint64		digit = (uint64) (*p - '0');

		if (val > (PG_UINT64_MAX - digit) / 10)
		{
			pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
			return false;
		}
		val = val * 10 + digit;
		p++;
	}

	if (val == 0)
	{
		pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
		return false;
	}

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	unit_start = p;
	while (*p != '\0' && !isspace((unsigned char) *p))
		p++;
	unit_len = (size_t) (p - unit_start);

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	if (*p != '\0')
	{
		pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
		return false;
	}

	if (unit_len == 0)
	{
		multiplier = (uint64) 1024 * 1024;
	}
	else if (unit_len == 1 ||
			 (unit_len == 2 &&
			  (unit_start[1] == 'b' || unit_start[1] == 'B')))
	{
		switch (unit_start[0])
		{
			case 'k':
			case 'K':
				multiplier = (uint64) 1024;
				break;
			case 'm':
			case 'M':
				multiplier = (uint64) 1024 * 1024;
				break;
			case 'g':
			case 'G':
				multiplier = (uint64) 1024 * 1024 * 1024;
				break;
			case 't':
			case 'T':
				multiplier = (uint64) 1024 * 1024 * 1024 * 1024;
				break;
			default:
				pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
				return false;
		}
	}
	else
	{
		pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
		return false;
	}

	if (val > PG_UINT64_MAX / multiplier)
	{
		pg_log_error("invalid value for %s: \"%s\"", optname, optarg);
		return false;
	}

	if (result)
		*result = val * multiplier;
	return true;
}

/*
 * option_parse_bool
 *
 * Parse a boolean value for an option (on/off, true/false, yes/no, 1/0,
 * case-insensitive, ignoring leading/trailing whitespace).
 * Returns true and stores the parsed value in *result on success; logs an
 * error and returns false on failure.
 */
bool
option_parse_bool(const char *optarg, const char *optname,
				  bool *result)
{
	const char *p = optarg;
	const char *start;
	size_t		len;

	if (optarg == NULL)
	{
		pg_log_error("invalid value \"%s\" for option %s", "", optname);
		return false;
	}

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	start = p;
	while (*p != '\0' && !isspace((unsigned char) *p))
		p++;
	len = (size_t) (p - start);

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	if (len == 0 || *p != '\0')
	{
		pg_log_error("invalid value \"%s\" for option %s", optarg, optname);
		return false;
	}

	if ((len == 2 && pg_strncasecmp(start, "on", 2) == 0) ||
		(len == 4 && pg_strncasecmp(start, "true", 4) == 0) ||
		(len == 3 && pg_strncasecmp(start, "yes", 3) == 0) ||
		(len == 1 && start[0] == '1'))
	{
		if (result)
			*result = true;
		return true;
	}

	if ((len == 3 && pg_strncasecmp(start, "off", 3) == 0) ||
		(len == 5 && pg_strncasecmp(start, "false", 5) == 0) ||
		(len == 2 && pg_strncasecmp(start, "no", 2) == 0) ||
		(len == 1 && start[0] == '0'))
	{
		if (result)
			*result = false;
		return true;
	}

	pg_log_error("invalid value \"%s\" for option %s", optarg, optname);
	return false;
}

/*
 * Provide strictly harmonized handling of the --sync-method option.
 */
bool
parse_sync_method(const char *optarg, DataDirSyncMethod *sync_method)
{
	if (strcmp(optarg, "fsync") == 0)
		*sync_method = DATA_DIR_SYNC_METHOD_FSYNC;
	else if (strcmp(optarg, "syncfs") == 0)
	{
#ifdef HAVE_SYNCFS
		*sync_method = DATA_DIR_SYNC_METHOD_SYNCFS;
#else
		pg_log_error("this build does not support sync method \"%s\"",
					 "syncfs");
		return false;
#endif
	}
	else
	{
		pg_log_error("unrecognized sync method: %s", optarg);
		return false;
	}

	return true;
}

/*
 * Fail with appropriate error if 2 or more of the specified options are set.
 * The first parameter is the number of arguments.  Following that is an
 * arbitrary number of bool/string pairs.  The bool indicates whether the
 * option is set, and the string is used for the error message.  Note that only
 * the first pair of enabled options we find are reported.
 *
 * Don't call this directly.  Use the check_mut_excl_opts() macro in
 * option_utils.h, which is the exact same except it doesn't take the first
 * parameter (it discovers the number of arguments automagically).
 */
void
check_mut_excl_opts_internal(int n, ...)
{
	char	   *first = NULL;
	va_list		args;

	Assert(n % 2 == 0);

	va_start(args, n);
	for (int i = 0; i < n; i += 2)
	{
		bool		set = va_arg(args, int);
		char	   *opt = va_arg(args, char *);

		if (set && first)
			pg_fatal("options %s and %s cannot be used together",
					 first, opt);

		if (set)
			first = opt;
	}
	va_end(args);
}
