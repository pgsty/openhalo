/* contrib/aux_mysql/sql/aux_mysql--1.9--2.0.sql */
-- Portions Copyright (c) 2026, PGSTY PTE. LTD.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- Upgrade aux_mysql 1.9 (OpenHalo 18.6.1) to 2.0 (OpenHalo 19.0.0)
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '2.0'" to load this file. \quit

SET LOCAL search_path = mysql, pg_catalog, pg_temp;

CREATE OR REPLACE FUNCTION mysql.anycompatiblecondandbytea(anycompatible, bytea)
 RETURNS boolean
 LANGUAGE c
 IMMUTABLE
AS '$libdir/aux_mysql', $function$anycompatibleCondAndByteaCImp$function$;

CREATE OR REPLACE FUNCTION mysql.byteacondandanycompatible(bytea, anycompatible)
 RETURNS boolean
 LANGUAGE c
 IMMUTABLE
AS '$libdir/aux_mysql', $function$byteaCondAndAnycompatibleCImp$function$;

CREATE OR REPLACE FUNCTION mysql.byteacondandbytea(bytea, bytea)
 RETURNS boolean
 LANGUAGE c
 IMMUTABLE
AS '$libdir/aux_mysql', $function$byteaCondAndByteaCImp$function$;

CREATE OR REPLACE FUNCTION mysql.clear_user_variables()
 RETURNS void
 LANGUAGE c
 STRICT
AS '$libdir/aux_mysql', $function$mysClearUserVariables$function$;

CREATE OR REPLACE FUNCTION mysql.curtime(prec integer)
 RETURNS time without time zone
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    ret time(6);
BEGIN
    if prec = 0 then
        ret = (select current_time(0));
    elsif prec = 1 then
        ret = (select current_time(1));
    elsif prec = 2 then
        ret = (select current_time(2));
    elsif prec = 3 then
        ret = (select current_time(3));
    elsif prec = 4 then
        ret = (select current_time(4));
    elsif prec = 5 then
        ret = (select current_time(5));
    elsif prec = 6 then
        ret = (select current_time(6));
    elsif prec < 0 then
        raise exception 'You have an error in your SQL syntax;';
    else
        raise exception 'Too-big precision % specified for ''curtime''. Maximum is 6.', prec;
    end if;

    return ret;
END
$function$;

CREATE OR REPLACE FUNCTION mysql.database()
 RETURNS text
 LANGUAGE c
 STABLE
AS '$libdir/aux_mysql', $function$mys_database$function$;

CREATE OR REPLACE FUNCTION mysql.found_rows()
 RETURNS bigint
 LANGUAGE c
 STRICT
AS '$libdir/aux_mysql', $function$mysFoundRows$function$;

CREATE OR REPLACE FUNCTION mysql.get_column_key(text, text, text)
 RETURNS text
 LANGUAGE plpgsql
 STABLE STRICT
AS $function$
declare
    ret pg_catalog.text;
    schemaName pg_catalog.varchar(128);
    tableName pg_catalog.varchar(128);
    columnName pg_catalog.varchar(128);
    indexs RECORD;
    columnIndexNum pg_catalog.int4;
BEGIN
    schemaName := $1;
    tableName := $2;
    columnName := $3;

    ret := '';

    FOR indexs IN
        select cl.relname, ind.indrelid, ind.indkey, ind.indnatts,
                ind.indnkeyatts, ind.indisunique, ind.indisprimary
            from pg_catalog.pg_class cl, pg_catalog.pg_index ind
            where (cl.oid = ind.indexrelid) and
                    (ind.indrelid = (select oid
                                        from pg_catalog.pg_class
                                        where (relname = tableName) and
                                                (relnamespace = (select oid
                                                                    from pg_catalog.pg_namespace
                                                                    where nspname = schemaName))))
    LOOP
        select count(*)
            from pg_catalog.pg_attribute
            where attrelid = indexs.indrelid
              and attnum = any(indexs.indkey)
              and attname = columnName
            into columnIndexNum;
        if (1 <= columnIndexNum) then
            if (indexs.indisprimary = 1) then
                ret := 'PRI';
                exit;
            else
                if (indexs.indisunique = 1) then
                    if (1 = array_length(string_to_array(indexs.indkey::text, ' '), 1)) then
                        ret := 'UNI';
                        exit;
                    else
                        ret := 'MUL';
                    end if;
                else
                    ret := 'MUL';
                end if;
            end if;
        end if;
    END LOOP;

    return ret;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.get_funcs_procs_parameters()
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
BEGIN
    RETURN QUERY
    SELECT
        'def'::varchar(256),
        p.pronamespace::regnamespace::text::varchar(256),
        p.proname::varchar(256),
        0::int,
        null::varchar(256),
        null::varchar(256),
        mysql.get_mysql_type_name(p.prorettype)::varchar(256),
        null::int,
        null::int,
        10::bigint,
        0::int,
        null::bigint,
        null::varchar(256),
        null::varchar(256),
        mysql.get_mysql_type_name(p.prorettype)::text,
        'FUNCTION'::varchar(256)
    FROM pg_catalog.pg_proc p
    WHERE 16384 <= p.oid AND p.prokind = 'f'
      AND p.pronamespace NOT IN ('pg_catalog'::regnamespace, 'mysql'::regnamespace)
    UNION ALL
    SELECT
        'def'::varchar(256),
        p.pronamespace::regnamespace::text::varchar(256),
        p.proname::varchar(256),
        a.ord::int,
        'IN'::varchar(256),
        p.proargnames[a.ord]::varchar(256),
        mysql.get_mysql_type_name(a.typid)::varchar(256),
        null::int,
        null::int,
        10::bigint,
        0::int,
        null::bigint,
        null::varchar(256),
        null::varchar(256),
        mysql.get_mysql_type_name(a.typid)::text,
        'FUNCTION'::varchar(256)
    FROM pg_catalog.pg_proc p
    CROSS JOIN LATERAL pg_catalog.unnest(p.proargtypes) WITH ORDINALITY AS a(typid, ord)
    WHERE 16384 <= p.oid AND p.prokind = 'f'
      AND p.pronamespace NOT IN ('pg_catalog'::regnamespace, 'mysql'::regnamespace)
    UNION ALL
    SELECT
        'def'::varchar(256),
        p.pronamespace::regnamespace::text::varchar(256),
        p.proname::varchar(256),
        a.ord::int,
        coalesce(mysql.get_mysql_proc_arg_mode(p.proargmodes[a.ord]), 'IN')::varchar(256),
        p.proargnames[a.ord]::varchar(256),
        mysql.get_mysql_type_name(a.typid)::varchar(256),
        null::int,
        null::int,
        10::bigint,
        0::int,
        null::bigint,
        null::varchar(256),
        null::varchar(256),
        mysql.get_mysql_type_name(a.typid)::text,
        'PROCEDURE'::varchar(256)
    FROM pg_catalog.pg_proc p
    CROSS JOIN LATERAL pg_catalog.unnest(coalesce(p.proallargtypes, p.proargtypes::oid[])) WITH ORDINALITY AS a(typid, ord)
    WHERE 16384 <= p.oid AND p.prokind = 'p'
      AND p.pronamespace NOT IN ('pg_catalog'::regnamespace, 'mysql'::regnamespace);
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.get_system_global_variable(text)
 RETURNS text
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    get_ret pg_catalog.text;
    system_var_name pg_catalog.text;
    is_session_sys_var pg_catalog.bool;
BEGIN
    system_var_name := $1;
    is_session_sys_var := false;

    get_ret := mysql.get_system_variable(system_var_name, is_session_sys_var);
    return get_ret;
END
$function$;

CREATE OR REPLACE FUNCTION mysql.get_system_session_variable(text)
 RETURNS text
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    get_ret pg_catalog.text;
    system_var_name pg_catalog.text;
    is_session_sys_var pg_catalog.bool;
BEGIN
    system_var_name := $1;
    is_session_sys_var := true;

    get_ret := mysql.get_system_variable(system_var_name, is_session_sys_var);
    return get_ret;
END
$function$;

CREATE OR REPLACE FUNCTION mysql.get_system_variable(text, boolean)
 RETURNS text
 LANGUAGE c
 STABLE STRICT
AS '$libdir/aux_mysql', $function$getSystemVariable$function$;

CREATE OR REPLACE FUNCTION mysql.load_file(text)
 RETURNS text
 LANGUAGE plpgsql
 STRICT
AS $function$
BEGIN
    return pg_catalog.pg_read_file($1);
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.locate(text, time without time zone, integer)
 RETURNS integer
 LANGUAGE plpgsql
 IMMUTABLE STRICT
AS $function$
BEGIN
    return mysql.text_locate3($1, $2::text, $3);
END
$function$;

CREATE OR REPLACE FUNCTION mysql.schema()
 RETURNS text
 LANGUAGE c
 STABLE
AS '$libdir/aux_mysql', $function$mys_database$function$;

CREATE OR REPLACE FUNCTION mysql.set_system_variable(text, text, boolean)
 RETURNS boolean
 LANGUAGE c
AS '$libdir/aux_mysql', $function$setSystemVariable$function$;

CREATE OR REPLACE FUNCTION mysql.set_user_var(variable_name character varying, variable_value anycompatible)
 RETURNS void
 LANGUAGE c
AS '$libdir/aux_mysql', $function$mysSetUserVar$function$;

CREATE OR REPLACE FUNCTION mysql.show_create_function(text, text)
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    schName pg_catalog.varchar(128);
    funcName pg_catalog.varchar(128);
    func RECORD;
    recordNum pg_catalog.int4;
BEGIN
    schName := $1;
    funcName := $2;
    recordNum := 0;

    FOR func IN
        select
            Name,
            @@sql_mode::varchar(256) as sql_mode,
            Define,
            Character_set_client,
            Collation_connection,
            Database_Collation
        FROM mys_informa_schema.functions
        WHERE Db = schName and Name = funcName
    LOOP
        recordNum := recordNum + 1;
        return next func;
    END LOOP;

    if (recordNum = 0) then
        raise exception 'FUNCTION % does not exist', funcName;
    end if;

    return;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_create_procedure(text, text)
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    schName pg_catalog.varchar(128);
    procName pg_catalog.varchar(128);
    proc RECORD;
    recordNum pg_catalog.int4;
BEGIN
    schName := $1;
    procName := $2;
    recordNum := 0;

    FOR proc IN
        select
            Name,
            @@sql_mode::varchar(256) as sql_mode,
            Define,
            Character_set_client,
            Collation_connection,
            Database_Collation
        FROM mys_informa_schema.procedures
        WHERE Db = schName and Name = procName
    LOOP
        recordNum := recordNum + 1;
        return next proc;
    END LOOP;

    if (recordNum = 0) then
        raise exception 'PROCEDURE % does not exist', procName;
    end if;

    return;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_create_table(text, text)
 RETURNS text
 LANGUAGE plpgsql
 STABLE
AS $function$
declare
    schName pg_catalog.text;
    tabName pg_catalog.text;
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    isTableOrView pg_catalog.int4;
    records RECORD;
BEGIN
    if ($1 is not null) then
        schName := $1;
    else
        schName := mysql.database();
        if (schName is null) then
            raise exception using errcode = 'M1046', message = 'No database selected';
        end if;
    end if;
    if (pg_catalog.lower(schName) = 'information_schema') then
        schName := 'mys_informa_schema';
    elsif (pg_catalog.lower(schName) = 'sys') then
        schName := 'mys_sys';
    end if;
    if ($2 is not null) then
        tabName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;
    isTableOrView := 0;

    FOR records IN
        select oid from pg_namespace where nspname = schName
    LOOP
        schOid := records.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;

    FOR records IN
        select viewname from pg_catalog.pg_views where (schemaname = schName) and (viewname = tabName)
    LOOP
        isTableOrView := 2;
    END LOOP;

    if (isTableOrView = 0) then
        FOR records IN
            select oid from pg_class where (relname = tabName) and (relnamespace = schOid)
        LOOP
            tabOid := records.oid;
            isTableOrView := 1;
        END LOOP;
    end if;

    if (isTableOrView = 1) then
        return mysql.show_create_table_impl(schOid, tabOid, schName, tabName);
    elsif (isTableOrView = 2) then
        return mysql.show_create_view_impl(schName, tabName);
    else
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_create_table_impl(oid, oid, text, text)
 RETURNS text
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    ret pg_catalog.text;
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    schName pg_catalog.text;
    tabName pg_catalog.text;
    columnIndex pg_catalog.int4;
    autoInc pg_catalog.int8;
    tmp pg_catalog.int4;
    tableComment pg_catalog.text;
    columnNames pg_catalog.text;
    columnNums pg_catalog.text;
    records RECORD;
BEGIN
    schOid := $1;
    tabOid := $2;
    schName := $3;
    tabName := $4;
    columnIndex := 0;

    ret := pg_catalog.concat('CREATE TABLE `', replace(tabName, '`', '``'), '` (');

    FOR records IN
        select att.attidentity as identity, mysql.column_extra(att.attrelid, att.attnum) COLLATE pg_catalog."C" as extra, att.attname as column_name,
                mysql.column_default(att.attrelid, att.attnum) as column_default,
                mysql.column_generation(att.attrelid, att.attnum) as generation,
                (case when att.attnotnull then 'NO' else 'YES' end) as is_nullable,
                (select
                    case
                        when ty.typname='tinyint' then 'tinyint'
                        when ty.typname='tinyint signed' then 'tinyint signed'
                        when ty.typname='tinyint unsigned' then 'tinyint unsigned'
                        when ty.typname='smallint' then 'smallint'
                        when ty.typname='smallint signed' then 'smallint signed'
                        when ty.typname='smallint unsigned' then 'smallint unsigned'
                        when ty.typname='mediumint' then 'mediumint'
                        when ty.typname='mediumint signed' then 'mediumint signed'
                        when ty.typname='mediumint unsigned' then 'mediumint unsigned'
                        when ty.typname='int4' then 'int(11)'
                        when ty.typname='int signed' then 'int signed'
                        when ty.typname='int unsigned' then 'int unsigned'
                        when ty.typname='int8' then 'bigint'
                        when ty.typname='bigint signed' then 'bigint signed'
                        when ty.typname='bigint unsigned' then 'bigint unsigned'
                        when ty.typname='varchar' then pg_catalog.concat('varchar', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character varying', ''))
                        when ty.typname='bpchar' then pg_catalog.concat('char', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character', ''))
                        when ty.typname='text' then 'text'
                        when ty.typname='date' then 'date'
                        when ty.typname='time' then 'time'
                        when ty.typname='datetime' then 'datetime(6)'
                        when ty.typname='timestamp' then (pg_catalog.concat('timestamp(', att.atttypmod, ')'))
                        when ty.typname='bit' then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                        when ty.typname='varbit' and att.atttypmod != -1 then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                        when ty.typname='varbit' and att.atttypmod = -1 then 'bit(1)'
                        when ty.typname='binary_n' then pg_catalog.concat('binary(', att.atttypmod, ')')
            when ty.typname='binary' then 'binary'
                        when ty.typname='varbinary_n' then pg_catalog.concat('varbinary(', att.atttypmod, ')')
            when ty.typname='varbinary' then 'varbinary'
                        when ty.typname='tinyblob' then 'tinyblob'
                        when ty.typname='blob' then 'blob'
                        when ty.typname='mediumblob' then 'mediumblob'
                        when ty.typname='longblob' then 'longblob'
                        when ty.typname='float8' then 'double'
            when mysql.left(ty.typname, 5)='float' then 'float'
                        when ty.typname='double' then 'double'
                        when ty.typname='decimal' then 'decimal'
                        when ty.oid='pg_catalog.numeric'::pg_catalog.regtype then mysql.decimal_column_type(att.atttypmod)
                        when ty.typname='real' then 'double'
                        when ty.typname='year_' then 'year'
                        else ty.typname
                    end
                    from pg_catalog.pg_type ty
                    where coalesce(mysql.column_domain(att.attrelid, att.attnum), att.atttypid) = ty.oid)::varchar(64) as column_type,
                col_description(att.attrelid, att.attnum) as column_comment,
                att.attnum::int4 as attnum,
                (select coll.collname from pg_collation coll where coll.oid = att.attcollation)::varchar(128) as column_collation
            from pg_catalog.pg_attribute att
            WHERE att.attrelid = tabOid and 0 < att.attnum and att.attisdropped = false
    LOOP
        if (0 < records.attnum) then
            if (0 < columnIndex) then
                ret := pg_catalog.concat(ret, E',\n  ');
            else
                ret := pg_catalog.concat(ret, E'\n  ');
            end if;

            ret := pg_catalog.concat(ret, '`');
            ret := pg_catalog.concat(ret, replace(records.column_name, '`', '``'));
            ret := pg_catalog.concat(ret, '`');
            ret := pg_catalog.concat(ret, ' ', records.column_type);
            if (records.generation is not null) then
                ret := pg_catalog.concat(ret, ' GENERATED ALWAYS AS (', records.generation, ') ',
                    pg_catalog.split_part(records.extra, ' ', 1));
            end if;
            if (records.is_nullable = 'NO') then
                ret := pg_catalog.concat(ret, ' NOT NULL');
            end if;
            if (records.identity != '') then
                ret := pg_catalog.concat(ret, ' AUTO_INCREMENT');
                BEGIN
                    if (pg_catalog.pg_get_serial_sequence(format('%I.%I', schName, tabName), records.column_name) is not null) then
                        execute format('select last_value + case when is_called then 1 else 0 end from %s',
                            pg_catalog.pg_get_serial_sequence(format('%I.%I', schName, tabName), records.column_name)) into autoInc;
                    end if;
                EXCEPTION WHEN insufficient_privilege THEN
                    autoInc := NULL;
                END;
            elsif (records.column_default is not null) then
                if (0 < position('nextval(' in records.column_default)) then
                    ret := pg_catalog.concat(ret, ' AUTO_INCREMENT');
                    execute format('select max(%I) from %I.%I', records.column_name, schName, tabName)
                    into autoInc;
                else
                    tmp := position('::character varying' in records.column_default);
                    if (0 < tmp) then
                        ret := pg_catalog.concat(ret, ' DEFAULT ', substring(records.column_default from 1 for (tmp - 1)));
                    else
                        ret := pg_catalog.concat(ret, ' DEFAULT ', records.column_default);
                    end if;
                end if;
            else
                if (records.is_nullable != 'NO' and records.generation is null) then
                    ret := pg_catalog.concat(ret, ' DEFAULT NULL');
                end if;
            end if;

            if (position('on update' in records.extra) > 0) then
                ret := pg_catalog.concat(ret, ' ', substring(records.extra from position('on update' in records.extra)));
            end if;
            if (records.column_collation is not null) then
                ret := pg_catalog.concat(ret, ' COLLATE ', records.column_collation);
            end if;

            if (records.column_comment is not null) then
                ret := pg_catalog.concat(ret, ' COMMENT ''', replace(records.column_comment, '''', ''''''), '''');
            end if;

            columnIndex := columnIndex + 1;
        end if;
    END LOOP;

    FOR records IN
        --select cl.relname, ind.indrelid, ind.indkey, ind.indnatts, ind.indnkeyatts, ind.indisunique, ind.indisprimary
        --    from pg_class cl,
        --        (select * from pg_index where indrelid = tabOid) ind
        --    where cl.oid = ind.indexrelid
        select ind.indexrelid, ind.indrelid, ind.indkey, ind.indnatts, ind.indnkeyatts, ind.indisunique, ind.indisprimary, mysql.index_key_name(ind.indexrelid) as indname
            from pg_index ind where ind.indrelid = tabOid
        --select ind.indrelid, ind.indkey, ind.indnatts, ind.indnkeyatts, ind.indisunique, ind.indisprimary, cl.relname
        --    from pg_index ind join pg_class cl on ind.indrelid = cl.oid
        --    where ind.indrelid = tabOid and cl.oid = tabOid
    LOOP
        select pg_catalog.string_agg(pg_catalog.concat('`', replace(k.column_name, '`', '``'), '`',
                case when k.sub_part is not null then pg_catalog.concat('(', k.sub_part, ')') end), ',' order by k.seq)
            from mysql.index_key_parts(records.indexrelid) k into columnNames;
        continue when columnNames is null;
        ret := pg_catalog.concat(ret, E',\n  ');
        --execute format('select GROUP_CONCAT(attname separator \'`,`\')
        --                    from pg_attribute
        --                    where attrelid = %s and attnum in (%s)',
        --               records.indrelid, columnNums)
        if (records.indisprimary = 1) then
            ret := pg_catalog.concat(ret, 'PRIMARY KEY (', columnNames, ')');
        elsif (records.indisunique = 1) then
            ret := pg_catalog.concat(ret, 'UNIQUE KEY `', replace(records.indname, '`', '``'), '` (', columnNames, ')');
        else
            ret := pg_catalog.concat(ret, 'KEY `', replace(records.indname, '`', '``'), '` (', columnNames, ')');
        end if;
    END LOOP;

    FOR records IN
        select
            cons.conname,
            --(select group_concat(att.attname separator ',') from pg_attribute att where (0 < array_position(cons.conkey, att.attnum)) and (att.attrelid = tabOId)) as column_name,
            (select pg_catalog.string_agg(replace(att.attname, '`', '``'), '`,`' order by k.ord) from pg_catalog.unnest(cons.conkey) with ordinality k(num,ord) join pg_catalog.pg_attribute att on att.attnum=k.num and att.attrelid=tabOId) as column_name,
            (select pg_catalog.concat(replace(n.nspname, '`', '``'), '`.`', replace(cl.relname, '`', '``')) from pg_catalog.pg_class cl join pg_catalog.pg_namespace n on n.oid=cl.relnamespace where cl.oid=cons.confrelid) as ref_table_name,
            --(select group_concat(att.attname separator ',') from pg_attribute att where (0 < array_position(cons.confkey, att.attnum)) and (att.attrelid = cons.confrelid)) as ref_column_name,
            (select pg_catalog.string_agg(replace(att.attname, '`', '``'), '`,`' order by k.ord) from pg_catalog.unnest(cons.confkey) with ordinality k(num,ord) join pg_catalog.pg_attribute att on att.attnum=k.num and att.attrelid=cons.confrelid) as ref_column_name,
            cons.confdeltype,
            cons.confupdtype
        from pg_constraint cons
        where (cons.contype = 'f') and (cons.conrelid = tabOid)
    LOOP
        ret := pg_catalog.concat(ret, E',\n  ');
        ret := pg_catalog.concat(ret, 'CONSTRAINT `');
        ret := pg_catalog.concat(ret, replace(records.conname, '`', '``'));
        ret := pg_catalog.concat(ret, '` FOREIGN KEY (`');
        ret := pg_catalog.concat(ret, records.column_name);
        ret := pg_catalog.concat(ret, '`) REFERENCES `');
        ret := pg_catalog.concat(ret, records.ref_table_name);
        ret := pg_catalog.concat(ret, '` (`');
        ret := pg_catalog.concat(ret, records.ref_column_name);
        ret := pg_catalog.concat(ret, '`) ON DELETE ');
        if (records.confdeltype = 'a') then
            ret := pg_catalog.concat(ret, 'RESTRICT ');
        elsif (records.confdeltype = 'c') then
            ret := pg_catalog.concat(ret, 'CASCADE ');
        elsif (records.confdeltype = 'd') then
            ret := pg_catalog.concat(ret, 'SET DEFAULT ');
        elsif (records.confdeltype = 'n') then
            ret := pg_catalog.concat(ret, 'SET NULL ');
        else
            ret := pg_catalog.concat(ret, 'RESTRICT ');
        end if;
        ret := pg_catalog.concat(ret, ' ON UPDATE ');
        if (records.confupdtype = 'a') then
            ret := pg_catalog.concat(ret, 'RESTRICT ');
        elsif (records.confupdtype = 'c') then
            ret := pg_catalog.concat(ret, 'CASCADE ');
        elsif (records.confupdtype = 'd') then
            ret := pg_catalog.concat(ret, 'SET DEFAULT ');
        elsif (records.confupdtype = 'n') then
            ret := pg_catalog.concat(ret, 'SET NULL ');
        else
            ret := pg_catalog.concat(ret, 'RESTRICT ');
        end if;
    END LOOP;

    FOR records IN SELECT d AS definition FROM mysql.pending_fk_definitions(tabOid) d LOOP
        ret := pg_catalog.concat(ret, E',\n  ', records.definition);
    END LOOP;

    FOR records IN
        select cons.conname, (select pg_get_constraintdef(cons.oid)) as consrc
        from pg_constraint cons
        where (cons.contype = 'c') and (cons.conrelid = tabOid) and cons.conname not like 'aux_mysql_domain_%'
    LOOP
        ret := pg_catalog.concat(ret, E',\n  ');
        ret := pg_catalog.concat(ret, 'CONSTRAINT ');
        ret := pg_catalog.concat(ret, replace(records.conname, '`', '``'));
        ret := pg_catalog.concat(ret, ' ');
        ret := pg_catalog.concat(ret, records.consrc);
    END LOOP;

    if (0 < autoInc) then
        -- identity metadata already supplies the next value
        ret := pg_catalog.concat(ret, E'\n) ENGINE=InnoDB AUTO_INCREMENT=', autoInc::text, ' DEFAULT CHARSET=utf8mb4');
    else
        ret := pg_catalog.concat(ret, E'\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4');
    end if;

    ret := pg_catalog.concat(ret, ' COLLATE=', coalesce((select case label when 'case_insensitive' then 'utf8mb4_general_ci' when 'case_sensitive' then 'utf8mb4_bin' else label end from pg_catalog.pg_seclabel
        where classoid = 'pg_class'::regclass and objoid = tabOid and objsubid = 0 and provider = 'aux_mysql'), 'utf8mb4_general_ci'));
    select coalesce(ds.description, '')
      into tableComment
      from pg_catalog.pg_description ds
     where ds.objoid = tabOid
       and ds.classoid = 'pg_catalog.pg_class'::pg_catalog.regclass
       and ds.objsubid = 0;
    tableComment := coalesce(tableComment, '');
    if (tableComment != '') then
        ret := pg_catalog.concat(ret, ' COMMENT ''', replace(tableComment, '''', ''''''), '''');
    end if;

    return ret;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_create_view(text, text)
 RETURNS text
 LANGUAGE plpgsql
 STABLE
AS $function$
declare
    schName pg_catalog.text;
    tabName pg_catalog.text;
    schOid pg_catalog.Oid;
    isTableOrView pg_catalog.int4;
    records RECORD;
BEGIN
    if ($1 is not null) then
        schName := $1;
    else
        schName := mysql.database();
        if (schName is null) then
            raise exception using errcode = 'M1046', message = 'No database selected';
        end if;
    end if;
    if (pg_catalog.lower(schName) = 'information_schema') then
        schName := 'mys_informa_schema';
    elsif (pg_catalog.lower(schName) = 'sys') then
        schName := 'mys_sys';
    end if;
    if ($2 is not null) then
        tabName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    isTableOrView := 0;

    FOR records IN
        select oid from pg_namespace where nspname = schName
    LOOP
        schOid := records.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;

    FOR records IN
        select viewname from pg_catalog.pg_views where (schemaname = schName) and (viewname = tabName)
    LOOP
        isTableOrView := 2;
    END LOOP;

    if (isTableOrView = 0) then
        FOR records IN
            select oid from pg_class where (relname = tabName) and (relnamespace = schOid)
        LOOP
            isTableOrView := 1;
        END LOOP;
    end if;

    if (isTableOrView = 1) then
        raise EXCEPTION '''%.%'' is not VIEW', schName, tabName;
    elsif (isTableOrView = 2) then
        return mysql.show_create_view_impl(schName, tabName);
    else
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_table_columns(text, text)
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    schemaName pg_catalog.varchar(128);
    tableName pg_catalog.varchar(128);
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    sch RECORD;
    tab RECORD;
    columns RECORD;
BEGIN
    if ($1 is not null) then
        schemaName := $1;
    else
        schemaName := mysql.database();
        if (schemaName is null) then
            raise exception using errcode = 'M1046', message = 'No database selected';
        end if;
    end if;
    if (pg_catalog.lower(schemaName) = 'information_schema') then
        schemaName := 'mys_informa_schema';
    elsif (pg_catalog.lower(schemaName) = 'sys') then
        schemaName := 'mys_sys';
    end if;
    if ($2 is not null) then
        tableName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;

    FOR sch IN
        select oid from pg_namespace where nspname = schemaName
    LOOP
        schOid := sch.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'invalid schema name';
    end if;

    FOR tab IN
        select oid from pg_class where (16384 <= oid) and (relname = tableName) and (relnamespace = schOid)
    LOOP
        tabOid := tab.oid;
    END LOOP;
    if (tabOid = 0) then
        raise EXCEPTION 'invalid table name';
    end if;

    FOR columns IN
        select
            att.attname::varchar(256) as Field,
            (select
                case
                    when ty.typname='tinyint' then 'tinyint'
                    when ty.typname='tinyint signed' then 'tinyint signed'
                    when ty.typname='tinyint unsigned' then 'tinyint unsigned'
                    when ty.typname='smallint' then 'smallint'
                    when ty.typname='smallint signed' then 'smallint signed'
                    when ty.typname='smallint unsigned' then 'smallint unsigned'
                    when ty.typname='mediumint' then 'mediumint'
                    when ty.typname='mediumint signed' then 'mediumint signed'
                    when ty.typname='mediumint unsigned' then 'mediumint unsigned'
                    when ty.typname='int4' then 'int(11)'
                    when ty.typname='int signed' then 'int signed'
                    when ty.typname='int unsigned' then 'int unsigned'
                    when ty.typname='int8' then 'bigint'
                    when ty.typname='bigint signed' then 'bigint signed'
                    when ty.typname='bigint unsigned' then 'bigint unsigned'
                    when ty.typname='varchar' then pg_catalog.concat('varchar', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character varying', ''))
                    when ty.typname='bpchar' then pg_catalog.concat('char', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character', ''))
                    when ty.typname='text' then 'text'
                    when ty.typname='date' then 'date'
                    when ty.typname='time' then 'time'
                    when ty.typname='datetime' then 'datetime(6)'
                    when ty.typname='timestamp' then (pg_catalog.concat('timestamp(', att.atttypmod, ')'))
                    when ty.typname='bit' then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod != -1 then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod = -1 then 'bit(1)'
                    when ty.typname='binary_n' then pg_catalog.concat('binary(', att.atttypmod, ')')
            when ty.typname='binary' then 'binary'
                    when ty.typname='varbinary_n' then pg_catalog.concat('varbinary(', att.atttypmod, ')')
            when ty.typname='varbinary' then 'varbinary'
                    when ty.typname='tinyblob' then 'tinyblob'
                    when ty.typname='blob' then 'blob'
                    when ty.typname='mediumblob' then 'mediumblob'
                    when ty.typname='longblob' then 'longblob'
                    when ty.typname='float8' then 'double'
            when mysql.left(ty.typname, 5)='float' then 'float'
                    when ty.typname='double' then 'double'
                    when ty.typname='decimal' then 'decimal'
                    when ty.oid='pg_catalog.numeric'::pg_catalog.regtype then mysql.decimal_column_type(att.atttypmod)
                    when ty.typname='real' then 'double'
                    when ty.typname='year_' then 'year'
                    else ty.typname
                end
                from pg_catalog.pg_type ty
                where att.atttypid = ty.oid)::varchar(64) as Type,
            (case when att.attnotnull then 'NO' else 'YES' end)::varchar(8) as Null,
            (mysql.get_column_key(att.attrelid, att.attname))::varchar(256) as Key,
            (mysql.amend_def_val(pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid and att.attgenerated = ''), tabOid)))::text as Default,
            mysql.column_extra(att.attrelid, att.attnum) COLLATE pg_catalog."C"::text as Extra
        FROM pg_catalog.pg_attribute att
        WHERE att.attrelid = tabOid and att.attname != 'tableoid' and att.attname != 'cmax' and
                att.attname != 'xmax' and att.attname != 'cmin' and att.attname != 'xmin' and
                att.attname != 'ctid' and att.attname not like '%pg.dropped.%'
                order by att.attnum
    LOOP
        return next columns;
    END LOOP;

    return;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_table_full_columns(text, text)
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    schemaName pg_catalog.varchar(128);
    tableName pg_catalog.varchar(128);
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    sch RECORD;
    tab RECORD;
    columns RECORD;
BEGIN
    if ($1 is not null) then
        schemaName := $1;
    else
        schemaName := mysql.database();
        if (schemaName is null) then
            raise exception using errcode = 'M1046', message = 'No database selected';
        end if;
    end if;
    if (pg_catalog.lower(schemaName) = 'information_schema') then
        schemaName := 'mys_informa_schema';
    elsif (pg_catalog.lower(schemaName) = 'sys') then
        schemaName := 'mys_sys';
    end if;
    if ($2 is not null) then
        tableName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;

    FOR sch IN
        select oid from pg_namespace where nspname = schemaName
    LOOP
        schOid := sch.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'invalid schema name';
    end if;

    FOR tab IN
        select oid from pg_class where (16384 <= oid) and (relname = tableName) and (relnamespace = schOid)
    LOOP
        tabOid := tab.oid;
    END LOOP;
    if (tabOid = 0) then
        raise EXCEPTION 'invalid table name';
    end if;

    FOR columns IN
        select
            att.attname::varchar(256) as Field,
            (select
                case
                    when ty.typname='tinyint' then 'tinyint'
                    when ty.typname='tinyint signed' then 'tinyint signed'
                    when ty.typname='tinyint unsigned' then 'tinyint unsigned'
                    when ty.typname='smallint' then 'smallint'
                    when ty.typname='smallint signed' then 'smallint signed'
                    when ty.typname='smallint unsigned' then 'smallint unsigned'
                    when ty.typname='mediumint' then 'mediumint'
                    when ty.typname='mediumint signed' then 'mediumint signed'
                    when ty.typname='mediumint unsigned' then 'mediumint unsigned'
                    when ty.typname='int4' then 'int(11)'
                    when ty.typname='int signed' then 'int signed'
                    when ty.typname='int unsigned' then 'int unsigned'
                    when ty.typname='int8' then 'bigint'
                    when ty.typname='bigint signed' then 'bigint signed'
                    when ty.typname='bigint unsigned' then 'bigint unsigned'
                    when ty.typname='varchar' then pg_catalog.concat('varchar', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character varying', ''))
                    when ty.typname='bpchar' then pg_catalog.concat('char', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character', ''))
                    when ty.typname='text' then 'text'
                    when ty.typname='date' then 'date'
                    when ty.typname='time' then 'time'
                    when ty.typname='datetime' then 'datetime(6)'
                    when ty.typname='timestamp' then (pg_catalog.concat('timestamp(', att.atttypmod, ')'))
                    when ty.typname='bit' then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod != -1 then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod = -1 then 'bit(1)'
                    when ty.typname='binary_n' then pg_catalog.concat('binary(', att.atttypmod, ')')
            when ty.typname='binary' then 'binary'
                    when ty.typname='varbinary_n' then pg_catalog.concat('varbinary(', att.atttypmod, ')')
            when ty.typname='varbinary' then 'varbinary'
                    when ty.typname='blob' then 'blob'
                    when ty.typname='mediumblob' then 'mediumblob'
                    when ty.typname='longblob' then 'longblob'
                    when ty.typname='float8' then 'double'
            when mysql.left(ty.typname, 5)='float' then 'float'
                    when ty.typname='double' then 'double'
                    when ty.typname='decimal' then 'decimal'
                    when ty.oid='pg_catalog.numeric'::pg_catalog.regtype then mysql.decimal_column_type(att.atttypmod)
                    when ty.typname='real' then 'double'
                    when ty.typname='year_' then 'year'
                    else ty.typname
                end
                from pg_catalog.pg_type ty
                where att.atttypid = ty.oid)::varchar(64) as Type,
            (select colla.collname from pg_catalog.pg_collation colla where colla.oid = att.attcollation)::varchar(256) as Collation,
            (case when att.attnotnull then 'NO' else 'YES' end)::varchar(8) as Null,
            (mysql.get_column_key(att.attrelid, att.attname))::varchar(256) as Key,
            (mysql.amend_def_val(pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid and att.attgenerated = ''), tabOid)))::text as Default,
            mysql.column_extra(att.attrelid, att.attnum) COLLATE pg_catalog."C"::text as Extra,
            'select,insert,update,references'::varchar(256) as Privileges,
            (case when col_description(att.attrelid, att.attnum) is not null then col_description(att.attrelid, att.attnum) else '' end)::varchar(512) as Comment
        FROM pg_catalog.pg_attribute att
        WHERE att.attrelid = tabOid and att.attname != 'tableoid' and att.attname != 'cmax' and
                att.attname != 'xmax' and att.attname != 'cmin' and att.attname != 'xmin' and
                att.attname != 'ctid' and att.attname not like '%pg.dropped.%'
                order by att.attnum
    LOOP
        return next columns;
    END LOOP;

    return;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_table_indexs(text, text)
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    schName pg_catalog.text;
    tabName pg_catalog.text;
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    keys pg_catalog.int4[];
    element pg_catalog.int4;
    records RECORD;
    subRecords RECORD;
BEGIN
    if ($1 is not null) then
        schName := $1;
    else
        schName := mysql.database();
        if (schName is null) then
            raise exception using errcode = 'M1046', message = 'No database selected';
        end if;
    end if;
    if (pg_catalog.lower(schName) = 'information_schema') then
        schName := 'mys_informa_schema';
    elsif (pg_catalog.lower(schName) = 'sys') then
        schName := 'mys_sys';
    end if;
    if ($2 is not null) then
        tabName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;

    FOR records IN
        select oid from pg_namespace where nspname = schName
    LOOP
        schOid := records.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;

    FOR records IN
        select oid from pg_class where (16384 <= oid) and (relname = tabName) and (relnamespace = schOid)
    LOOP
        tabOid := records.oid;
    END LOOP;
    if (tabOid = 0) then
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;

    FOR records IN
        select ind.indexrelid, ind.indkey, ind.indisunique FROM pg_catalog.pg_index ind WHERE ind.indrelid = tabOid
    LOOP
            FOR subRecords IN
                select
                    (select cl.relname from pg_class cl where (cl.oid = tabOid))::varchar(256) as table_name,
                    (case when records.indisunique = 1 then 0 else 1 end)::int as Non_unique,
                    mysql.index_key_name(records.indexrelid)::varchar(256) as key_name,
                    k.seq::int as Seq_in_index,
                    att.attname::varchar(256) as Column_name,
                    'A'::varchar(256) as Collation,
                    0::int as Cardinality,
                    k.sub_part::int as Sub_part,
                    NULL::varchar(256) as Packed,
                    (case when att.attnotnull then '' else 'YES' end)::varchar(8) as _null,
                    pg_catalog.upper((select case when am.amname='mysql_prefix' then 'btree' else am.amname end from pg_am am where am.oid = (select cl.relam from pg_class cl where cl.oid = records.indexrelid)))::varchar(256) as Index_type,
                    ''::varchar(512) as _comment,
                    ''::varchar(512) as Index_comment
                from mysql.index_key_parts(records.indexrelid) k
                    join pg_catalog.pg_attribute att on att.attrelid = tabOid and att.attname = k.column_name
                order by k.seq
            LOOP
                return next subRecords;
            END LOOP;
    END LOOP;

    --XFOR records IN
    --X    select con.conrelid, con.conname, con.conkey FROM pg_catalog.pg_constraint con WHERE con.conrelid = tabOid and con.contype = 'f'
    --XLOOP
    --X    FOREACH element IN ARRAY records.conkey LOOP
    --X        FOR subRecords IN
    --X            select
    --X                (select cl.relname from pg_class cl where (cl.oid = records.conrelid))::varchar(256) as table_name,
    --X                1::int as Non_unique,
    --X                records.conname::varchar(256) as key_name,
    --X                1::int as Seq_in_index,
    --X                (select att.attname from pg_attribute att where (att.attnum = records.conkey[1]) and (att.attrelid = tabOid))::varchar(256) as Column_name,
    --X                'A'::varchar(256) as Collation,
    --X                0::int as Cardinality,
    --X                NULL::int as Sub_part,
    --X                NULL::varchar(256) as Packed,
    --X                ''::varchar(8) as _null,
    --X                'BTREE'::varchar(256) as Index_type,
    --X                ''::varchar(512) as _comment,
    --X                ''::varchar(512) as Index_comment
    --X            from pg_catalog.pg_attribute att
    --X            where att.attrelid = tabOid and att.attnum = element
    --X        LOOP
    --X            return next subRecords;
    --X        END LOOP;
    --X    END LOOP;
    --XEND LOOP;

    return;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.show_table_indexs_for_8(text, text)
 RETURNS SETOF record
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    schName pg_catalog.text;
    tabName pg_catalog.text;
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    keys pg_catalog.int4[];
    element pg_catalog.int4;
    records RECORD;
    subRecords RECORD;
BEGIN
    if ($1 is not null) then
        schName := $1;
    else
        schName := mysql.database();
        if (schName is null) then
            raise exception using errcode = 'M1046', message = 'No database selected';
        end if;
    end if;
    if (pg_catalog.lower(schName) = 'information_schema') then
        schName := 'mys_informa_schema';
    elsif (pg_catalog.lower(schName) = 'sys') then
        schName := 'mys_sys';
    end if;
    if ($2 is not null) then
        tabName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;

    FOR records IN
        select oid from pg_namespace where nspname = schName
    LOOP
        schOid := records.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;

    FOR records IN
        select oid from pg_class where (16384 <= oid) and (relname = tabName) and (relnamespace = schOid)
    LOOP
        tabOid := records.oid;
    END LOOP;
    if (tabOid = 0) then
        raise EXCEPTION 'Table ''%.%'' doesn''t exist', schName, tabName;
    end if;

    FOR records IN
        select ind.indexrelid, ind.indkey, ind.indisunique FROM pg_catalog.pg_index ind WHERE ind.indrelid = tabOid
    LOOP
            FOR subRecords IN
                select
                    (select cl.relname from pg_class cl where (cl.oid = tabOid))::varchar(256) as table_name,
                    (case when records.indisunique = 1 then 0 else 1 end)::int as Non_unique,
                    mysql.index_key_name(records.indexrelid)::varchar(256) as key_name,
                    k.seq::int as Seq_in_index,
                    att.attname::varchar(256) as Column_name,
                    'A'::varchar(256) as Collation,
                    0::int as Cardinality,
                    k.sub_part::int as Sub_part,
                    NULL::varchar(256) as Packed,
                    (case when att.attnotnull then '' else 'YES' end)::varchar(8) as _null,
                    pg_catalog.upper((select case when am.amname='mysql_prefix' then 'btree' else am.amname end from pg_am am where am.oid = (select cl.relam from pg_class cl where cl.oid = records.indexrelid)))::varchar(256) as Index_type,
                    ''::varchar(512) as _comment,
                    ''::varchar(512) as Index_comment,
                    'YES'::varchar(64) as Visible,
                    NULL::varchar(512) as Expression
                from mysql.index_key_parts(records.indexrelid) k
                    join pg_catalog.pg_attribute att on att.attrelid = tabOid and att.attname = k.column_name
                order by k.seq
            LOOP
                return next subRecords;
            END LOOP;
    END LOOP;

    --XFOR records IN
    --X    select con.conrelid, con.conname, con.conkey FROM pg_catalog.pg_constraint con WHERE con.conrelid = tabOid and con.contype = 'f'
    --XLOOP
    --X    FOREACH element IN ARRAY records.conkey LOOP
    --X        FOR subRecords IN
    --X            select
    --X                (select cl.relname from pg_class cl where (cl.oid = records.conrelid))::varchar(256) as table_name,
    --X                1::int as Non_unique,
    --X                records.conname::varchar(256) as key_name,
    --X                1::int as Seq_in_index,
    --X                (select att.attname from pg_attribute att where (att.attnum = records.conkey[1]) and (att.attrelid = tabOid))::varchar(256) as Column_name,
    --X                'A'::varchar(256) as Collation,
    --X                0::int as Cardinality,
    --X                NULL::int as Sub_part,
    --X                NULL::varchar(256) as Packed,
    --X                ''::varchar(8) as _null,
    --X                'BTREE'::varchar(256) as Index_type,
    --X                ''::varchar(512) as _comment,
    --X                ''::varchar(512) as Index_comment
    --X            from pg_catalog.pg_attribute att
    --X            where att.attrelid = tabOid and att.attnum = element
    --X        LOOP
    --X            return next subRecords;
    --X        END LOOP;
    --X    END LOOP;
    --XEND LOOP;

    return;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.utc_date()
 RETURNS date
 LANGUAGE sql
 STABLE
AS $function$
    SELECT (CURRENT_TIMESTAMP::pg_catalog.timestamptz AT TIME ZONE 'UTC')::date
$function$;

CREATE OR REPLACE FUNCTION mysql.utc_time()
 RETURNS time without time zone
 LANGUAGE sql
 STABLE
AS $function$
    SELECT (CURRENT_TIMESTAMP::pg_catalog.timestamptz AT TIME ZONE 'UTC')::time(0)
$function$;

CREATE OR REPLACE FUNCTION mysql.utc_time(prec integer)
 RETURNS time without time zone
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    tmp time(6);
    ret time(6);
BEGIN
    tmp = (SELECT (CURRENT_TIMESTAMP(6)::pg_catalog.timestamptz AT TIME ZONE 'UTC')::time(6));
    if prec = 0 then
        ret = tmp::time(0);
    elsif prec = 1 then
        ret = tmp::time(1);
    elsif prec = 2 then
        ret = tmp::time(2);
    elsif prec = 3 then
        ret = tmp::time(3);
    elsif prec = 4 then
        ret = tmp::time(4);
    elsif prec = 5 then
        ret = tmp::time(5);
    elsif prec = 6 then
        ret = tmp::time(6);
    elsif prec < 0 then
        raise exception 'You have an error in your SQL syntax;';
    else
        raise exception 'Too-big precision % specified for ''utc_time''. Maximum is 6.', prec;
    end if;

    return ret;
END
$function$;

CREATE OR REPLACE FUNCTION mysql.utc_timestamp()
 RETURNS timestamp without time zone
 LANGUAGE sql
 STABLE
AS $function$
    SELECT (CURRENT_TIMESTAMP::pg_catalog.timestamptz AT TIME ZONE 'UTC')::timestamp(0)
$function$;

CREATE OR REPLACE FUNCTION mysql.utc_timestamp(prec integer)
 RETURNS timestamp without time zone
 LANGUAGE plpgsql
 STABLE
AS $function$
DECLARE
    tmp timestamp(6);
    ret timestamp(6);
BEGIN
    tmp = (SELECT (CURRENT_TIMESTAMP(6)::pg_catalog.timestamptz AT TIME ZONE 'UTC')::timestamp(6));
    if prec = 0 then
        ret = tmp::timestamp(0);
    elsif prec = 1 then
        ret = tmp::timestamp(1);
    elsif prec = 2 then
        ret = tmp::timestamp(2);
    elsif prec = 3 then
        ret = tmp::timestamp(3);
    elsif prec = 4 then
        ret = tmp::timestamp(4);
    elsif prec = 5 then
        ret = tmp::timestamp(5);
    elsif prec = 6 then
        ret = tmp::timestamp(6);
    elsif prec < 0 then
        raise exception 'You have an error in your SQL syntax;';
    else
        raise exception 'Too-big precision % specified for ''utc_timestamp''. Maximum is 6.', prec;
    end if;

    return ret;
END
$function$;

CREATE OR REPLACE VIEW mys_informa_schema.indexs AS
 SELECT (( SELECT ns.nspname
           FROM pg_namespace ns
          WHERE ns.oid = (( SELECT cl_sub.relnamespace
                   FROM pg_class cl_sub
                  WHERE cl_sub.oid = ind.indrelid))))::character varying(256) AS schema_name,
    (( SELECT cl_sub.relname
           FROM pg_class cl_sub
          WHERE cl_sub.oid = ind.indrelid))::character varying(256) AS table_name,
        CASE
            WHEN ind.indisunique OPERATOR(mysql.=) 1 THEN 0
            ELSE 1
        END AS non_unique,
    mysql.index_key_name(ind.indexrelid)::character varying(256) AS key_name,
    mysql.get_seq_in_index(att.attnum::text, ind.indkey)::integer AS seq_in_index,
    att.attname::character varying(256) AS column_name,
    'A'::character varying(256) AS "collation",
    0 AS cardinality,
    NULL::integer AS sub_part,
    NULL::character varying(256) AS packed,
        CASE
            WHEN att.attnotnull THEN ''::text
            ELSE 'YES'::text
        END AS _null,
    upper((( SELECT
                CASE
                    WHEN am.amname = 'mysql_prefix'::name THEN 'btree'::name
                    ELSE am.amname
                END AS amname
           FROM pg_am am,
            pg_class cl_sub
          WHERE am.oid = cl_sub.relam AND cl_sub.oid = ind.indexrelid))::text)::character varying(256) AS index_type,
    ''::character varying(256) AS _comment,
    ((( SELECT ds.description
           FROM pg_description ds
          WHERE ds.objoid = cl.oid AND ds.classoid = 'pg_class'::regclass::oid AND ds.objsubid = 0)))::character varying(512) AS index_comment
   FROM pg_attribute att,
    pg_index ind,
    pg_class cl
  WHERE (att.attnum = ANY (ind.indkey::smallint[])) AND att.attrelid = cl.oid AND ind.indrelid = cl.oid
UNION
 SELECT (( SELECT ns.nspname
           FROM pg_namespace ns
          WHERE ns.oid = cons.connamespace))::character varying(256) AS schema_name,
    (( SELECT cl.relname
           FROM pg_class cl
          WHERE cl.oid = cons.conrelid))::character varying(256) AS table_name,
    1 AS non_unique,
    cons.conname::character varying(256) AS key_name,
    1 AS seq_in_index,
    (( SELECT att.attname
           FROM pg_attribute att
          WHERE att.attnum = cons.conkey[1] AND att.attrelid = cons.conrelid))::character varying(256) AS column_name,
    'A'::character varying(256) AS "collation",
    0 AS cardinality,
    NULL::integer AS sub_part,
    NULL::character varying(256) AS packed,
    ''::text AS _null,
    'BTREE'::character varying(256) AS index_type,
    ''::character varying(256) AS _comment,
    ''::character varying(256) AS index_comment
   FROM pg_constraint cons
  WHERE cons.contype = 'f'::"char";

CREATE OR REPLACE VIEW mys_informa_schema.tables AS
 SELECT 'def'::character varying(256) AS table_catalog,
    (( SELECT ns.nspname
           FROM pg_namespace ns
          WHERE ns.oid = cl.relnamespace))::character varying(256) AS table_schema,
    relname::character varying(256) AS table_name,
        CASE
            WHEN relkind = 'v'::"char" THEN 'VIEW'::text
            ELSE 'BASE TABLE'::text
        END::character varying(256) AS table_type,
        CASE
            WHEN relkind <> 'v'::"char" THEN 'InnoDB'::text
            ELSE NULL::text
        END::character varying(256) AS engine,
        CASE
            WHEN relkind <> 'v'::"char" THEN 10
            ELSE NULL::integer
        END::bigint AS version,
        CASE
            WHEN relkind <> 'v'::"char" THEN 'Dynamic'::text
            ELSE NULL::text
        END::character varying(256) AS row_format,
        CASE
            WHEN relkind <> 'v'::"char" THEN reltuples
            ELSE NULL::real
        END::bigint AS table_rows,
        CASE
            WHEN relkind <> 'v'::"char" THEN 100
            ELSE NULL::integer
        END::bigint AS avg_row_length,
        CASE
            WHEN relkind <> 'v'::"char" THEN 16384
            ELSE NULL::integer
        END::bigint AS data_length,
        CASE
            WHEN relkind <> 'v'::"char" THEN 16384
            ELSE NULL::integer
        END::bigint AS max_data_length,
        CASE
            WHEN relkind <> 'v'::"char" THEN 16384
            ELSE NULL::integer
        END::bigint AS index_length,
        CASE
            WHEN relkind <> 'v'::"char" THEN 16384
            ELSE NULL::integer
        END::bigint AS data_free,
        CASE
            WHEN relkind <> 'v'::"char" THEN ( SELECT count(*) AS count
               FROM pg_class
              WHERE pg_class.relname ~~ mysql.concat(cl.relname, '%') AND pg_class.relkind = 'S'::"char")
            ELSE NULL::bigint
        END AS auto_increment,
    NULL::bigint AS create_time,
    NULL::bigint AS update_time,
    NULL::timestamp without time zone AS check_time,
        CASE
            WHEN relkind <> 'v'::"char" THEN 'utf8mb4_general_ci'::text
            ELSE NULL::text
        END::character varying(256) AS table_collation,
    NULL::bigint AS checksum,
        CASE
            WHEN relkind <> 'v'::"char" THEN ''::text
            ELSE NULL::text
        END::character varying(256) AS create_options,
        CASE
            WHEN relkind = 'v'::"char" THEN 'VIEW'::text
            ELSE ( SELECT ds.description
               FROM pg_description ds
              WHERE ds.objoid = cl.oid AND ds.classoid = 'pg_class'::regclass::oid AND ds.objsubid = 0)
        END::character varying(512) AS table_comment
   FROM pg_class cl
  WHERE oid >= 16384::oid AND (relkind = ANY (ARRAY['r'::"char", 'p'::"char", 'v'::"char"]))
  ORDER BY ('def'::character varying(256));

-- Remove obsolete temporary scratch table after replacing get_funcs_procs_parameters()
DROP TABLE mys_informa_schema.parameters_tmp;

-- Restore MySQL boolean/integer cast contexts (also lost across pg_upgrade)
UPDATE pg_catalog.pg_cast SET castcontext = 'i' WHERE castsource = 23 AND casttarget = 16;
UPDATE pg_catalog.pg_cast SET castcontext = 'a' WHERE castsource = 16 AND casttarget = 23;

-- Replace TIMEDIFF overloads to return formatted MySQL TIME text (supports negative and >24h)
DROP FUNCTION IF EXISTS mysql.timediff(timestamp, timestamp);
DROP FUNCTION IF EXISTS mysql.timediff(time, time);
DROP FUNCTION IF EXISTS mysql.timediff(text, text);
CREATE OR REPLACE FUNCTION mysql.timediff(text, text)
RETURNS text
LANGUAGE c
IMMUTABLE STRICT PARALLEL SAFE
AS '$libdir/aux_mysql', $function$mys_timediff_text$function$;

CREATE OR REPLACE FUNCTION mysql.timediff(time, time)
RETURNS text
AS $$
    SELECT mysql.timediff($1::text, $2::text)
$$ IMMUTABLE STRICT PARALLEL SAFE LANGUAGE SQL;

CREATE OR REPLACE FUNCTION mysql.timediff(timestamp, timestamp)
RETURNS text
AS $$
    SELECT mysql.timediff($1::text, $2::text)
$$ IMMUTABLE STRICT PARALLEL SAFE LANGUAGE SQL;

-- Fix O(N^2) PL/pgSQL loop in mysql.space() and cap at max_allowed_packet default (64MB)
CREATE OR REPLACE FUNCTION mysql.space(pg_catalog.int4)
RETURNS text
LANGUAGE sql
IMMUTABLE STRICT PARALLEL SAFE
AS
$$
    SELECT CASE
        WHEN $1 <= 0 THEN ''::pg_catalog.text
        WHEN $1 > 67108864 THEN NULL::pg_catalog.text
        ELSE pg_catalog.repeat(' ', $1)
    END
$$;

-- Fix broken PL/pgSQL string literals and @@sql_mode in routine inspection helpers
CREATE OR REPLACE FUNCTION mysql.get_proc_def(funcName text,
                                              proallargtypes oid[],
                                              proargmodes "char"[],
                                              proargnames text[],
                                              prosrc text)
RETURNS text
AS
$$
DECLARE
    ret text;
    allArgNum int;
    argIndex int;
BEGIN
    ret := 'CREATE PROCEDURE';

    ret := ret || ' ';
    ret := ret || funcName;

    ret := ret || '(';
    if (proallargtypes is not null) then
        allArgNum := array_length(proallargtypes, 1);
        argIndex := 1;
        LOOP
            if (1 < argIndex) then
                ret := ret || ',';
            end if;

            if (proargmodes[argIndex] = 'i') then
                ret := ret || ' IN';
            else
                ret := ret || ' OUT';
            end if;

            ret := ret || ' ';
            ret := ret || coalesce(proargnames[argIndex], '');

            ret := ret || ' ';
            ret := ret || proallargtypes[argIndex]::regType::text;

            argIndex := argIndex + 1;
            EXIT WHEN argIndex > allArgNum;
        END LOOP;
    end if;
    ret := ret || E')\n';

    ret := ret || prosrc;

    RETURN ret;
END
$$
IMMUTABLE LANGUAGE PLPGSQL;

CREATE OR REPLACE FUNCTION mysql.show_create_function(pg_catalog.text, pg_catalog.text)
RETURNS setof RECORD
AS
$$
DECLARE
    schName pg_catalog.varchar(128);
    funcName pg_catalog.varchar(128);
    func RECORD;
    recordNum pg_catalog.int4;
BEGIN
    schName := $1;
    funcName := $2;
    recordNum := 0;

    FOR func IN
        select
            Name,
            mysql.get_system_session_variable('sql_mode')::varchar(256) as sql_mode,
            Define,
            Character_set_client,
            Collation_connection,
            Database_Collation
        FROM mys_informa_schema.functions
        WHERE Db = schName and Name = funcName
    LOOP
        recordNum := recordNum + 1;
        return next func;
    END LOOP;

    if (recordNum = 0) then
        raise exception 'FUNCTION % does not exist', funcName;
    end if;

    return;
END;
$$
STABLE LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION mysql.show_create_procedure(pg_catalog.text, pg_catalog.text)
RETURNS setof RECORD
AS
$$
DECLARE
    schName pg_catalog.varchar(128);
    procName pg_catalog.varchar(128);
    proc RECORD;
    recordNum pg_catalog.int4;
BEGIN
    schName := $1;
    procName := $2;
    recordNum := 0;

    FOR proc IN
        select
            Name,
            mysql.get_system_session_variable('sql_mode')::varchar(256) as sql_mode,
            Define,
            Character_set_client,
            Collation_connection,
            Database_Collation
        FROM mys_informa_schema.procedures
        WHERE Db = schName and Name = procName
    LOOP
        recordNum := recordNum + 1;
        return next proc;
    END LOOP;

    if (recordNum = 0) then
        raise exception 'PROCEDURE % does not exist', procName;
    end if;

    return;
END;
$$
STABLE LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION mysql.get_proc_def(procName pg_catalog.text,
                                                procOwner pg_catalog.text,
                                                proargtypes pg_catalog.oidvector,
                                                proallargtypes pg_catalog.oid[],
                                                proargmodes "char"[],
                                                proargnames pg_catalog.text[],
                                                prosrc pg_catalog.text)
RETURNS pg_catalog.text
AS
$$
DECLARE
    ret pg_catalog.text;
    allArgNum pg_catalog.int4;
    argIndex pg_catalog.int4;
    argTypes pg_catalog.int4[];
BEGIN
    ret := 'CREATE DEFINER=`';
    ret := pg_catalog.concat(ret, replace(procOwner, '`', '``'));
    ret := pg_catalog.concat(ret, '`@`%` PROCEDURE `');
    ret := pg_catalog.concat(ret, replace(procName, '`', '``'));
    ret := pg_catalog.concat(ret, '`(');
    if (proallargtypes is not null) then
        allArgNum := array_length(proallargtypes, 1);
        argIndex := 1;
        LOOP
            if (1 < argIndex) then
                ret := pg_catalog.concat(ret, ', ');
            end if;

            if (proargmodes[argIndex] = 'i') then
                ret := pg_catalog.concat(ret, 'IN');
            elsif (proargmodes[argIndex] = 'b') then
                ret := pg_catalog.concat(ret, 'INOUT');
            else
                ret := pg_catalog.concat(ret, 'OUT');
            end if;

            ret := pg_catalog.concat(ret, ' ');
            ret := pg_catalog.concat(ret, proargnames[argIndex]);

            ret := pg_catalog.concat(ret, ' ');
            ret := pg_catalog.concat(ret, proallargtypes[argIndex]::regType::pg_catalog.text);

            argIndex := argIndex + 1;

            EXIT WHEN argIndex > allArgNum;
        END LOOP;
    else
        argTypes := string_to_array(proargtypes::text, ' ');
        allArgNum := array_length(argTypes, 1);
        if (0 < allArgNum) then
            argIndex := 1;
            LOOP
                if (1 < argIndex) then
                    ret := pg_catalog.concat(ret, ',');
                end if;

                ret := pg_catalog.concat(ret, ' IN');

                ret := pg_catalog.concat(ret, ' ');
                ret := pg_catalog.concat(ret, proargnames[argIndex]);

                ret := pg_catalog.concat(ret, ' ');
                ret := pg_catalog.concat(ret, argTypes[argIndex]::regType::pg_catalog.text);

                argIndex := argIndex + 1;
                EXIT WHEN argIndex > allArgNum;
            END LOOP;
        end if;
    end if;
    ret := pg_catalog.concat(ret, E')\n');

    ret := pg_catalog.concat(ret, prosrc);

    RETURN ret;
END
$$
IMMUTABLE LANGUAGE PLPGSQL;

CREATE OR REPLACE FUNCTION mysql.get_func_def(funcName text,
                                              procOwner pg_catalog.text,
                                              prorettype pg_catalog.oid,
                                              proargtypes pg_catalog.oidvector,
                                              proargnames pg_catalog.text[],
                                              prosrc text)
RETURNS text
AS
$$
DECLARE
    ret pg_catalog.text;
    argTypes pg_catalog.int4[];
    allArgNum pg_catalog.int4;
    argIndex pg_catalog.int4;
BEGIN
    ret := 'CREATE DEFINER=`';
    ret := pg_catalog.concat(ret, replace(procOwner, '`', '``'));
    ret := pg_catalog.concat(ret, '`@`%` FUNCTION `');
    ret := pg_catalog.concat(ret, replace(funcName, '`', '``'));
    ret := pg_catalog.concat(ret, '`(');
    argTypes := string_to_array(proargtypes::text, ' ');
    allArgNum := array_length(argTypes, 1);
    argIndex := 1;
    if (0 < allArgNum) then
        LOOP
            if (1 < argIndex) then
                ret := pg_catalog.concat(ret, ', ');
            end if;

            ret := pg_catalog.concat(ret, proargnames[argIndex]);
            ret := pg_catalog.concat(ret, ' ');
            ret := pg_catalog.concat(ret, argTypes[argIndex]::regType::text);

            argIndex := argIndex + 1;
            EXIT WHEN argIndex > allArgNum;
        END LOOP;
    end if;
    ret := pg_catalog.concat(ret, ')');

    ret := pg_catalog.concat(ret, ' ');
    ret := pg_catalog.concat(ret, 'RETURNS ', prorettype::regType::text);

    ret := pg_catalog.concat(ret, E'\n');
    ret := pg_catalog.concat(ret, prosrc);

    RETURN ret;
END
$$
IMMUTABLE LANGUAGE PLPGSQL;

-- Replace superuser-only pg_statistic / pg_authid reads in information_schema views
CREATE OR REPLACE VIEW mys_informa_schema.partitions AS
SELECT
'def'::varchar(512) as TABLE_CATALOG,
n.nspname::varchar(64) AS TABLE_SCHEMA,
p.relname::varchar(64) AS TABLE_NAME,
c.relname::varchar(64) AS PARTITION_NAME,
NULL::varchar(64) AS SUBPARTITION_NAME,
rank() over(partition by p.relname order by c.oid) AS PARTITION_ORDINAL_POSITION,
NULL::bigint AS SUBPARTITION_ORDINAL_POSITION,
CASE pg_partitioned_table.partstrat
WHEN 'r'::char THEN 'RANGE'::varchar(18)
WHEN 'l'::char THEN 'LIST'::varchar(18)
WHEN 'h'::char THEN 'HASH'::varchar(18)
ELSE 'UNKNOWN'::varchar(18)
END AS PARTITION_METHOD,
NULL::varchar(12) AS SUBPARTITION_METHOD,
NULL::text AS PARTITION_EXPRESSION,
NULL::text AS SUBPARTITION_EXPRESSION,
NULL::text AS PARTITION_DESCRIPTION,
c.reltuples::bigint AS TABLE_ROWS,
avg_row.stawidth::bigint AS AVG_ROW_LENGTH,
0::bigint AS DATA_LENGTH,
NULL::bigint AS MAX_DATA_LENGTH,
0::bigint AS INDEX_LENGTH,
0::bigint AS DATA_FREE,
NULL::mysql.datetime AS CREATE_TIME,
NULL::mysql.datetime AS UPDATE_TIME,
NULL::mysql.datetime AS CHECK_TIME,
NULL::bigint AS CHECKSUM,
''::varchar(80) AS PARTITION_COMMENT,
'default'::varchar(12) AS NODEGROUP,
coalesce(t.spcname::text, 'PG_DEFAULT')::varchar(64) as TABLESPACE_NAME
FROM pg_inherits i
JOIN pg_partitioned_table ON i.inhparent = pg_partitioned_table.partrelid
JOIN pg_class p ON i.inhparent = p.oid
JOIN pg_class c ON i.inhrelid = c.oid
LEFT JOIN pg_namespace n ON n.oid = c.relnamespace
LEFT JOIN (SELECT parent.oid, parent.relname AS partitioned_table_name,
    count(child.oid) AS num_partitions
    FROM pg_class parent
    LEFT JOIN pg_inherits ON pg_inherits.inhparent = parent.oid
    LEFT JOIN pg_class child ON pg_inherits.inhrelid = child.oid
    WHERE parent.relkind = 'p'
    GROUP BY parent.oid, parent.relname, parent.relkind) par on par.oid = i.inhrelid
LEFT JOIN pg_tablespace t on p.reltablespace = t.oid
LEFT JOIN (select i.inhparent,  sum(c.relpages) as blocks
    from pg_class c LEFT JOIN pg_inherits i ON c.oid=i.inhrelid
    where c.relispartition is true group by 1) s on c.oid = s.inhparent
LEFT JOIN (select schemaname, tablename, sum(avg_width) as stawidth
    FROM pg_catalog.pg_stats
    group by schemaname, tablename) avg_row
    on n.nspname = avg_row.schemaname and c.relname = avg_row.tablename
WHERE p.relispartition = false AND p.relkind NOT IN ('i', 'I') order by 1, 2, 3;

CREATE OR REPLACE VIEW mys_informa_schema.functions AS
SELECT
    (select ns.nspname from pg_namespace ns where ns.oid = pc.pronamespace)::varchar(256) as Db,
    pc.proname::varchar(256) as Name,
    'FUNCTION'::varchar(256) as Type,
    (mysql.get_func_def(pc.proname, pc.proowner::regrole::pg_catalog.text, pc.prorettype, pc.proargtypes, pc.proargnames, pc.prosrc))::text as Define,
    pg_catalog.pg_get_userbyid(pc.proowner)::varchar(256) as Definer,
    null::varchar(256) as Modified,
    null::varchar(256) as Created,
    'DEFINER'::varchar(256) as Security_type,
    ''::varchar(512) as Comment,
    'utf8mb4'::varchar(128) as Character_set_client,
    'utf8mb4_general_ci'::varchar(128) as Collation_connection,
    'utf8mb4_general_ci'::varchar(128) as Database_Collation
FROM pg_proc pc
WHERE pc.prokind = 'f' and substring(pc.proname from 1 for 21) != 'func_reset_serial_for';

CREATE OR REPLACE VIEW mys_informa_schema.procedures AS
SELECT
    (select ns.nspname from pg_namespace ns where ns.oid = pc.pronamespace)::varchar(256) as Db,
    pc.proname::varchar(256) as Name,
    'PROCEDURE'::varchar(256) as Type,
    (mysql.get_proc_def(pc.proname, pc.proowner::regrole::pg_catalog.text, pc.proargtypes, pc.proallargtypes, pc.proargmodes, pc.proargnames, pc.prosrc))::text as Define,
    pg_catalog.pg_get_userbyid(pc.proowner)::varchar(256) as Definer,
    '2022-10-21 10:20:30'::varchar(256) as Modified,
    '2022-10-20 10:20:30'::varchar(256) as Created,
    'DEFINER'::varchar(256) as Security_type,
    ''::varchar(512) as Comment,
    'utf8mb4'::varchar(128) as Character_set_client,
    'utf8mb4_general_ci'::varchar(128) as Collation_connection,
    'utf8mb4_general_ci'::varchar(128) as Database_Collation
FROM pg_proc pc
WHERE pc.prokind = 'p' and substring(pc.proname from 1 for 21) != 'func_reset_serial_for';

-- Fix volatility & parallel safety for session-stateful / warning-emitting functions
ALTER FUNCTION mysql.current_user() STABLE PARALLEL RESTRICTED;
ALTER FUNCTION mysql.session_user() STABLE PARALLEL RESTRICTED;

DO $$
DECLARE
    rec record;
BEGIN
    FOR rec IN
        SELECT p.oid::pg_catalog.regprocedure AS sig
        FROM pg_catalog.pg_depend d
        JOIN pg_catalog.pg_extension e
          ON e.oid = d.refobjid AND e.extname = 'aux_mysql'
        JOIN pg_catalog.pg_proc p
          ON p.oid = d.objid AND d.classid = 'pg_catalog.pg_proc'::pg_catalog.regclass
        JOIN pg_catalog.pg_namespace n
          ON n.oid = p.pronamespace
        WHERE d.deptype = 'e'
          AND n.nspname = 'mysql'
          AND p.provolatile = 's'
          AND p.proparallel = 's'
          AND p.proname NOT IN ('time_to_timestamp', 'concat', 'concat_ws')
    LOOP
        EXECUTE pg_catalog.format('ALTER FUNCTION %s PARALLEL RESTRICTED', rec.sig);
    END LOOP;
END;
$$;

-- Reconcile legacy PUBLIC execute grants on PostgreSQL file-read functions
REVOKE EXECUTE ON FUNCTION pg_catalog.pg_read_file(pg_catalog.text, pg_catalog.int8, pg_catalog.int8, pg_catalog.bool) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_catalog.pg_read_file(pg_catalog.text, pg_catalog.int8, pg_catalog.int8) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_catalog.pg_read_file(pg_catalog.text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_catalog.pg_read_binary_file(pg_catalog.text, pg_catalog.int8, pg_catalog.int8, pg_catalog.bool) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_catalog.pg_read_binary_file(pg_catalog.text, pg_catalog.int8, pg_catalog.int8) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_catalog.pg_read_binary_file(pg_catalog.text) FROM PUBLIC;
REVOKE ALL ON FUNCTION mysql.load_file(pg_catalog.text) FROM PUBLIC;

-- Reconcile extension-owned table/view ACLs without touching non-extension user tables
DO $$
DECLARE
    rec record;
BEGIN
    FOR rec IN
        SELECT n.nspname, c.relname
        FROM pg_catalog.pg_depend d
        JOIN pg_catalog.pg_extension e
          ON e.oid = d.refobjid AND e.extname = 'aux_mysql'
        JOIN pg_catalog.pg_class c
          ON c.oid = d.objid AND d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass
        JOIN pg_catalog.pg_namespace n
          ON n.oid = c.relnamespace
        WHERE d.deptype = 'e'
          AND c.relkind IN ('r', 'v', 'm', 'f', 'p')
          AND n.nspname IN ('mysql', 'mys_informa_schema', 'mys_sys', 'performance_schema')
    LOOP
        EXECUTE pg_catalog.format('REVOKE ALL ON TABLE %I.%I FROM PUBLIC', rec.nspname, rec.relname);
        IF rec.nspname = 'mysql' AND rec.relname = 'dual' THEN
            EXECUTE 'GRANT SELECT, REFERENCES ON TABLE mysql.dual TO PUBLIC';
        ELSE
            EXECUTE pg_catalog.format('GRANT SELECT ON TABLE %I.%I TO PUBLIC', rec.nspname, rec.relname);
        END IF;
    END LOOP;
END;
$$;
