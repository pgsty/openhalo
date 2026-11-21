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

    ret := pg_catalog.concat('CREATE TABLE `', tabName, '` (');

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
            ret := pg_catalog.concat(ret, records.column_name);
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
        select pg_catalog.string_agg(pg_catalog.concat('`', k.column_name, '`',
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
            ret := pg_catalog.concat(ret, 'UNIQUE KEY `', records.indname, '` (', columnNames, ')');
        else
            ret := pg_catalog.concat(ret, 'KEY `', records.indname, '` (', columnNames, ')');
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
