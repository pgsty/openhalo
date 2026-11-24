/*-------------------------------------------------------------------------
 *
 * partListColumns.c
 *	  Tuple key formatter for MySQL PARTITION BY LIST COLUMNS emulation
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"
#include "halo_mysql.h"

#include "catalog/pg_type.h"
#include "common/shortest_dec.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

typedef struct PartKeyColumn
{
	Oid			type;
	FmgrInfo	output;
} PartKeyColumn;

static PartKeyColumn *part_key_columns(FunctionCallInfo fcinfo);

/*
 * Append a single non-NULL tuple element using PostgreSQL record_out quoting
 * rules: NULL is emitted as empty, empty string as "", and any element
 * containing delimiters, quotes, backslashes, or whitespace is quoted with
 * doubled '"' and '\'. Normal scalar tokens (e.g. 1, foo) remain unquoted.
 */
static void
append_part_elem(StringInfo str, const char *val, size_t len)
{
	bool		needquote = (len == 0);

	for (size_t j = 0; j < len && !needquote; j++)
	{
		unsigned char ch = (unsigned char) val[j];

		if (ch == '"' || ch == '\\' || ch == ',' || ch == '(' || ch == ')' ||
			scanner_isspace(ch))
			needquote = true;
	}

	if (!needquote)
	{
		appendBinaryStringInfo(str, val, (int) len);
		return;
	}

	appendStringInfoChar(str, '"');
	for (size_t j = 0; j < len; j++)
	{
		char		ch = val[j];

		if (ch == '"' || ch == '\\')
			appendStringInfoChar(str, ch);
		appendStringInfoChar(str, ch);
	}
	appendStringInfoChar(str, '"');
}

/* Partition bounds and inserts may be evaluated with different session GUCs. */
static void
append_part_value(StringInfo str, PartKeyColumn *col, Datum value)
{
	char		buf[MAXDATELEN + 1];
	const int	utc = 0;

	switch (col->type)
	{
		case DATEOID:
		case TIMEOID:
		case TIMETZOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			JsonEncodeDateTime(buf, value, col->type, &utc);
			break;
		case FLOAT4OID:
			{
				float4		v = DatumGetFloat4(value);

				float_to_shortest_decimal_buf(v == 0 ? 0 : v, buf);
				break;
			}
		case FLOAT8OID:
			{
				float8		v = DatumGetFloat8(value);

				double_to_shortest_decimal_buf(v == 0 ? 0 : v, buf);
				break;
			}
		case INTERVALOID:
			{
				Interval   *span = DatumGetIntervalP(value);

				if (INTERVAL_NOT_FINITE(span))
					strlcpy(buf, INTERVAL_IS_NOBEGIN(span) ? EARLY : LATE, sizeof(buf));
				else
				{
					/* Same 30-day months as interval_cmp_value(). Keep whole
					 * days separate so the full interval range fits in int64. */
					int64		days = (int64) span->month * DAYS_PER_MONTH +
						span->day + span->time / USECS_PER_DAY;
					int64		usecs = span->time % USECS_PER_DAY;

					if (usecs < 0)
					{
						days--;
						usecs += USECS_PER_DAY;
					}
					snprintf(buf, sizeof(buf), INT64_FORMAT "d+" INT64_FORMAT "us",
							 days, usecs);
				}
				break;
			}
		case NUMERICOID:
			{
				Numeric		v = DatumGetNumeric(value);
				char	   *out = numeric_normalize(v);

				appendStringInfoString(str, out);
				pfree(out);
				if ((Pointer) v != DatumGetPointer(value))
					pfree(v);
				return;
			}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			{
				text	   *v = DatumGetTextPP(value);
				int			len = VARSIZE_ANY_EXHDR(v);

				if (col->type == BPCHAROID)
					len = bpchartruelen(VARDATA_ANY(v), len);
				append_part_elem(str, VARDATA_ANY(v), len);
				if ((Pointer) v != DatumGetPointer(value))
					pfree(v);
				return;
			}
		case BYTEAOID:
			{
				bytea	   *v = DatumGetByteaPP(value);
				int			len = VARSIZE_ANY_EXHDR(v);

				/* A quoted, record-escaped bytea hex string. */
				appendStringInfoString(str, "\"\\\\x");
				if ((Size) len > MaxAllocSize / 2)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("partition key is too large")));
				enlargeStringInfo(str, len * 2);
				str->len += hex_encode(VARDATA_ANY(v), len, str->data + str->len);
				appendStringInfoChar(str, '"');
				if ((Pointer) v != DatumGetPointer(value))
					pfree(v);
				return;
			}
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case UNKNOWNOID:
		case UUIDOID:
		case OIDOID:
		case NAMEOID:
			{
				char	   *out = OutputFunctionCall(&col->output, value);

				append_part_elem(str, out, strlen(out));
				pfree(out);
				return;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("type %s has no canonical LIST COLUMNS key",
							format_type_be(col->type))));
	}
	append_part_elem(str, buf, strlen(buf));
}

PG_FUNCTION_INFO_V1(partListColumns);
Datum
partListColumns(PG_FUNCTION_ARGS)
{
	text	   *result;
	StringInfoData str;
	PartKeyColumn *cols = fcinfo->flinfo->fn_extra;

	if (cols == NULL)
		cols = part_key_columns(fcinfo);
	initStringInfo(&str);
	appendStringInfoChar(&str, '(');
	for (int i = 0; i < PG_NARGS(); i++)
	{
		if (i > 0)
			appendStringInfoChar(&str, ',');
		if (!PG_ARGISNULL(i))
			append_part_value(&str, &cols[i], PG_GETARG_DATUM(i));
	}
	appendStringInfoChar(&str, ')');

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);
	PG_RETURN_TEXT_P(result);
}

static PartKeyColumn *
part_key_columns(FunctionCallInfo fcinfo)
{
	MemoryContext cxt = fcinfo->flinfo->fn_mcxt;
	PartKeyColumn *cols = MemoryContextAllocZero(cxt, Max(PG_NARGS(), 1) * sizeof(*cols));

	for (int i = 0; i < PG_NARGS(); i++)
	{
		Oid			type = get_fn_expr_argtype(fcinfo->flinfo, i);
		Oid			output;
		bool		varlena;

		if (!OidIsValid(type))
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of partition key")));
		cols[i].type = mys_binary_base_type(type);
		getTypeOutputInfo(cols[i].type, &output, &varlena);
		fmgr_info_cxt(output, &cols[i].output, cxt);
	}
	fcinfo->flinfo->fn_extra = cols;
	return cols;
}
