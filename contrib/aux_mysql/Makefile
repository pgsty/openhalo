# Portions Copyright (c) 2026, PGSTY PTE. LTD.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
# contrib/aux_mysql/Makefile
#
# aux_mysql: MySQL wire protocol & dialect for PostgreSQL 19 (needs the
# protocol extension hooks of the OpenHalo 19 kernel).  Built and installed
# by the top-level "make && make install"; also builds with PGXS against an
# installed OpenHalo 19 kernel:  make USE_PGXS=1 PG_CONFIG=.../pg_config

MODULE_big = aux_mysql
OBJS = \
	src/halo_mysql.o \
	src/mysql_proto.o \
	src/mys_auth.o \
	src/mys_stmt.o \
	src/mys_txn.o \
	src/mys_current_read.o \
	src/mys_semantics.o \
	src/mysql_parser.o \
	src/mys_compat.o \
	src/mys_lock.o \
	src/mys_dml.o \
	src/mys_prefix.o \
	src/mys_fk.o \
	src/mys_rewrite.o \
	src/mys_utility.o \
	src/mys_binary.o \
	src/parser/mys_gram.o \
	src/parser/mys_scan.o \
	src/parser/mys_parser.o \
	src/parser/mys_keywords.o \
	src/mysm/aggregates.o \
	src/mysm/bpchar.o \
	src/mysm/date_func.o \
	src/mysm/mys_date.o \
	src/mysm/mys_timestamp.o \
	src/mysm/mys_uservar.o \
	src/mysm/operators.o \
	src/mysm/partListColumns.o \
	src/mysm/strfuncs.o \
	src/mysm/systemVar.o \
	src/mysm/sysvar_core.o \
	src/mysm/time_func.o \
	src/mysm/timestamp_func.o \
	src/mysm/user.o \
	src/mysm/user_var.o \
	src/mysm/uuid_short.o \
	src/mysm/uuid_short_core.o

EXTENSION = aux_mysql
DATA = sql/aux_mysql--2.0.sql sql/aux_mysql--1.9--2.0.sql
PGFILEDESC = "aux_mysql - MySQL compatibility (openHalo)"

# generated grammar / keyword headers live in the build directory
PG_CPPFLAGS = -Isrc/parser -I$(srcdir)/src -I$(srcdir)/src/parser \
	-I$(srcdir)/src/mysm/include -DHALO_MYSQL_GRAMMAR

EXTRA_CLEAN = src/parser/mys_gram.c src/parser/mys_gram.h src/parser/mys_gram.output \
	src/parser/mys_scan.c src/parser/mys_kwlist_d.h

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/aux_mysql
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# TLS and RSA key exchange for caching_sha2_password
SHLIB_LINK += $(filter -lssl -lcrypto, $(LIBS))

src/parser/mys_gram.c: src/parser/mys_gram.y
	@$(MKDIR_P) src/parser
	$(BISON) -d -Wno-deprecated -Wno-other -o $@ $<

src/parser/mys_gram.h: src/parser/mys_gram.c
	@touch $@

src/parser/mys_scan.c: src/parser/mys_scan.l
	@$(MKDIR_P) src/parser
	$(FLEX) -Cfe -p -p -o'$@' $<

src/parser/mys_kwlist_d.h: src/parser/mys_kwlist.h tools/gen_keywordlist.pl tools/PerfectHash.pm
	@$(MKDIR_P) src/parser
	$(PERL) -I $(srcdir)/tools $(srcdir)/tools/gen_keywordlist.pl --extern --varname=MysScanKeywords --output src/parser $<

src/mysql_parser.o src/mys_stmt.o src/parser/mys_gram.o src/parser/mys_scan.o src/parser/mys_parser.o src/parser/mys_keywords.o: \
	src/parser/mys_gram.h src/parser/mys_kwlist_d.h

# PGXS tracks header dependencies only with --enable-depend: the shared
# structs (MysStmtInfo, mys_yy_extra_type, ...) must not get out of sync between objects.
$(OBJS): src/halo_mysql.h src/mys_proto.h src/mys_binary.h src/parser/mys_gramparse.h
src/mys_prefix.o src/mys_rewrite.o src/mys_binary.o src/halo_mysql.o: src/mys_prefix.h
src/mys_prefix.o: src/mys_general_ci_weights.h
src/mys_fk.o src/mys_utility.o: src/mys_fk.h
src/mys_current_read.o src/mys_utility.o: src/mys_current_read.h
src/halo_mysql.o src/mysm/sysvar_core.o src/halo_mysql.bc src/mysm/sysvar_core.bc: src/openhalo_version.h
