#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(31)

--!./tcltestrunner.lua
-- 2005 Mar 16
--
-- The author disclaims copyright to this source code.  In place of
-- a legal notice, here is a blessing:
--
--    May you do good and not evil.
--    May you find forgiveness for yourself and forgive others.
--    May you share freely, never taking more than you give.
--
-------------------------------------------------------------------------
-- This file implements regression tests for sql library.
--
-- This file implements tests for miscellanous features that were
-- left out of other test files.
--
-- $Id: misc5.test,v 1.22 2008/07/29 10:26:45 danielk1977 Exp $
-- ["set","testdir",[["file","dirname",["argv0"]]]]
-- ["source",[["testdir"],"\/tester.tcl"]]
-- Build records using the MakeRecord opcode such that the size of the 
-- header is at the transition point in the size of a varint.
--
-- This test causes an assertion failure or a buffer overrun in version
-- 3.1.5 and earlier.
--
for i = 120, 140 - 1, 1 do
    test:do_test(
        "misc5-1."..i,
        function()
            test:catchsql("DROP TABLE t1")
            local sql1 = "CREATE TABLE t1 (id  INT primary key,"
            local sql2 = "INSERT INTO t1 VALUES (1, "
            local sep = ""
            for j = 0, i - 1, 1 do
                sql1 = sql1 .. string.format("%sa%s INT", sep, j)
                sql2 = sql2 .. string.format("%s%s", sep, j)
                sep = ","
            end
            sql1 = sql1 .. ");"
            sql2 = sql2 .. ");"
            return test:execsql(string.format("%s%s", sql1, sql2))
        end, {
            
        })

end
-- # Make sure large integers are stored correctly.
-- #
-- ifcapable conflict {
--   do_test misc5-2.1 {
--     execsql {
--       create table t2(x  INT primary key);
--       insert into t2 values(1);
--       insert or ignore into t2 select x*2 from t2;
--       insert or ignore into t2 select x*4 from t2;
--       insert or ignore into t2 select x*16 from t2;
--       insert or ignore into t2 select x*256 from t2;
--       insert or ignore into t2 select x*65536 from t2;
--       insert or ignore into t2 select x*2147483648 from t2;
--       insert or ignore into t2 select x-1 from t2;
--       insert or ignore into t2 select x+1 from t2;
--       insert or ignore into t2 select -x from t2;
--       select count(*) from t2;
--     }
--   } 371
-- } else {
test:do_execsql_test(
    "misc5-2.1",
    [[
        create table t2(x  INT primary key);
        create table t2_temp(id  INT primary key, x INT );
        START TRANSACTION;
        insert into t2_temp values(1, 1);
        insert into t2_temp select id+1,x*2 from t2_temp;
        insert into t2_temp select id+2,x*4 from t2_temp;
        insert into t2_temp select id+4,x*16 from t2_temp;
        insert into t2_temp select id+8,x*256 from t2_temp;
        insert into t2_temp select id+16,x*65536 from t2_temp;
        insert into t2_temp select id+32,x*2147483648 from t2_temp;
        insert into t2_temp select id+64,x-1 from t2_temp;
        insert into t2_temp select id+128,x+1 from t2_temp;
        insert into t2_temp select id+256,-x from t2_temp;
        INSERT INTO t2 SELECT DISTINCT(x) FROM t2_temp;
        COMMIT;
        DROP TABLE t2_temp;
        select count(*) from t2;
    ]], {
        -- <misc5-2.1>
        371
        -- </misc5-2.1>
    })

--}
test:do_execsql_test(
    "misc5-2.2",
    [[
        select x from t2 order by x;
    ]], {
        -- <misc5-2.2>
    -4611686018427387905LL, -4611686018427387904LL, -4611686018427387903LL, -2305843009213693953LL, -2305843009213693952LL,
    -2305843009213693951LL, -1152921504606846977LL, -1152921504606846976LL, -1152921504606846975LL, -576460752303423489LL,
    -576460752303423488LL, -576460752303423487LL, -288230376151711745LL, -288230376151711744LL, -288230376151711743LL,
    -144115188075855873LL, -144115188075855872LL, -144115188075855871LL, -72057594037927937LL, -72057594037927936LL,
    -72057594037927935LL, -36028797018963969LL, -36028797018963968LL, -36028797018963967LL, -18014398509481985LL,
    -18014398509481984LL, -18014398509481983LL, -9007199254740993LL, -9007199254740992LL, -9007199254740991LL,
    -4503599627370497LL, -4503599627370496LL, -4503599627370495LL, -2251799813685249LL, -2251799813685248LL, -2251799813685247LL,
    -1125899906842625LL, -1125899906842624LL, -1125899906842623LL, -562949953421313LL, -562949953421312LL, -562949953421311LL,
    -281474976710657LL, -281474976710656LL, -281474976710655LL, -140737488355329LL, -140737488355328LL, -140737488355327LL,
    -70368744177665, -70368744177664, -70368744177663, -35184372088833, -35184372088832, -35184372088831,
    -17592186044417, -17592186044416, -17592186044415, -8796093022209, -8796093022208, -8796093022207,
    -4398046511105, -4398046511104, -4398046511103, -2199023255553, -2199023255552, -2199023255551, -1099511627777,
    -1099511627776, -1099511627775, -549755813889, -549755813888, -549755813887, -274877906945, -274877906944,
    -274877906943, -137438953473, -137438953472, -137438953471, -68719476737, -68719476736, -68719476735,
    -34359738369, -34359738368, -34359738367, -17179869185, -17179869184, -17179869183, -8589934593,
    -8589934592, -8589934591, -4294967297, -4294967296, -4294967295, -2147483649, -2147483648, -2147483647,
    -1073741825, -1073741824, -1073741823, -536870913, -536870912, -536870911, -268435457, -268435456, -268435455,
    -134217729, -134217728, -134217727, -67108865, -67108864, -67108863, -33554433, -33554432, -33554431, -16777217,
    -16777216, -16777215, -8388609, -8388608, -8388607, -4194305, -4194304, -4194303, -2097153, -2097152, -2097151,
    -1048577, -1048576, -1048575, -524289, -524288, -524287, -262145, -262144, -262143, -131073, -131072, -131071,
    -65537, -65536, -65535, -32769, -32768, -32767, -16385, -16384, -16383, -8193, -8192, -8191, -4097, -4096, -4095,
    -2049, -2048, -2047, -1025, -1024, -1023, -513, -512, -511, -257, -256, -255, -129, -128, -127, -65, -64, -63,
    -33, -32, -31, -17, -16, -15, -9, -8, -7, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
    63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097,
    8191, 8192, 8193, 16383, 16384, 16385, 32767, 32768, 32769, 65535, 65536, 65537, 131071, 131072, 131073, 262143,
    262144, 262145, 524287, 524288, 524289, 1048575, 1048576, 1048577, 2097151, 2097152, 2097153, 4194303, 4194304,
    4194305, 8388607, 8388608, 8388609, 16777215, 16777216, 16777217, 33554431, 33554432, 33554433, 67108863, 67108864,
    67108865, 134217727, 134217728, 134217729, 268435455, 268435456, 268435457, 536870911, 536870912, 536870913,
    1073741823, 1073741824, 1073741825, 2147483647, 2147483648, 2147483649, 4294967295, 4294967296, 4294967297,
    8589934591, 8589934592, 8589934593, 17179869183, 17179869184, 17179869185, 34359738367, 34359738368, 34359738369,
    68719476735, 68719476736, 68719476737, 137438953471, 137438953472, 137438953473, 274877906943, 274877906944,
    274877906945, 549755813887, 549755813888, 549755813889, 1099511627775, 1099511627776, 1099511627777, 2199023255551,
    2199023255552, 2199023255553, 4398046511103, 4398046511104, 4398046511105, 8796093022207, 8796093022208,
    8796093022209, 17592186044415, 17592186044416, 17592186044417, 35184372088831, 35184372088832, 35184372088833,
    70368744177663, 70368744177664, 70368744177665, 140737488355327LL, 140737488355328LL, 140737488355329LL, 281474976710655LL,
    281474976710656LL, 281474976710657LL, 562949953421311LL, 562949953421312LL, 562949953421313LL, 1125899906842623LL,
    1125899906842624LL, 1125899906842625LL, 2251799813685247LL, 2251799813685248LL, 2251799813685249LL, 4503599627370495LL,
    4503599627370496LL, 4503599627370497LL, 9007199254740991LL, 9007199254740992LL, 9007199254740993LL, 18014398509481983LL,
    18014398509481984LL, 18014398509481985LL, 36028797018963967LL, 36028797018963968LL, 36028797018963969LL, 72057594037927935LL,
    72057594037927936LL, 72057594037927937LL, 144115188075855871LL, 144115188075855872LL, 144115188075855873LL,
    288230376151711743LL, 288230376151711744LL, 288230376151711745LL, 576460752303423487LL, 576460752303423488LL,
    576460752303423489LL, 1152921504606846975LL, 1152921504606846976LL, 1152921504606846977LL, 2305843009213693951LL,
    2305843009213693952LL, 2305843009213693953LL, 4611686018427387903LL, 4611686018427387904LL, 4611686018427387905LL
        -- </misc5-2.2>
    })

-- Ticket #1210.  Do proper reference counting of Table structures
-- so that deeply nested SELECT statements can be flattened correctly.
--
test:do_execsql_test(
    "misc5-3.1",
    [[
        CREATE TABLE songs(songid  INT primary key, artist TEXT, timesplayed INT );
        INSERT INTO songs VALUES(1,'one',1);
        INSERT INTO songs VALUES(2,'one',2);
        INSERT INTO songs VALUES(3,'two',3);
        INSERT INTO songs VALUES(4,'three',5);
        INSERT INTO songs VALUES(5,'one',7);
        INSERT INTO songs VALUES(6,'two',11);
        SELECT DISTINCT artist 
        FROM (    
         SELECT DISTINCT artist    
         FROM songs      
         WHERE songid IN (    
          SELECT songid    
          FROM songs    
          WHERE LOWER(artist) IN (
            SELECT DISTINCT LOWER(artist)    
            FROM (      
              -- This sub-query returns the table:
              --
              --     two      14
              --     one      10
              --     three    5
              --
              SELECT DISTINCT artist,sum(timesplayed) AS total      
              FROM songs      
              GROUP BY LOWER(artist)      
              ORDER BY total DESC      
              LIMIT 10    
            )    
            WHERE artist <> ''
          )
         )
         LIMIT 1
        )  
        ORDER BY LOWER(artist) ASC;
    ]], {
        -- <misc5-3.1>
        "one"
        -- </misc5-3.1>
    })



-- # Ticket #1370.  Do not overwrite small files (less than 1024 bytes)
-- # when trying to open them as a database.
-- #
-- if {[permutation] == ""} {
--   do_test misc5-4.1 {
--     db close
--     forcedelete test.db
--     set fd [open test.db w]
--     puts $fd "This is not really a database"
--     close $fd
--     sql db test.db
--     catchsql {
--       CREATE TABLE t1(a INT ,b INT ,c INT );
--     }
--   } {1 {file is encrypted or is not a database}}
-- }
-- Ticket #1371.  Allow floating point numbers of the form .N  or N.
--
test:do_execsql_test(
    "misc5-5.1",
    [[
        SELECT .1 
    ]], {
        -- <misc5-5.1>
        0.1
        -- </misc5-5.1>
    })

test:do_execsql_test(
    "misc5-5.2",
    [[
        SELECT 2. 
    ]], {
        -- <misc5-5.2>
        2.0
        -- </misc5-5.2>
    })

test:do_execsql_test(
    "misc5-5.3",
    [[
        SELECT 3.e0 
    ]], {
        -- <misc5-5.3>
        3.0
        -- </misc5-5.3>
    })

test:do_execsql_test(
    "misc5-5.4",
    [[
        SELECT .4e+1
    ]], {
        -- <misc5-5.4>
        4.0
        -- </misc5-5.4>
    })

-- Ticket #1582.  Ensure that an unknown table in a LIMIT clause applied to
-- a UNION ALL query causes an error, not a crash.
--
-- db close
-- forcedelete test.db
-- sql db test.db
test:drop_all_tables()
-- do_test misc5-6.1 {
--   catchsql {
--     SELECT * FROM sql_master
--     UNION ALL 
--     SELECT * FROM sql_master
--     LIMIT (SELECT count(*) FROM blah);
--   }
-- } {1 {no such table: blah}}
-- do_test misc5-6.2 {
--   execsql {
--     CREATE TABLE logs(msg TEXT, timestamp INTEGER, dbtime TEXT);
--   }
--   catchsql {
--     SELECT * FROM logs WHERE logs.oid >= (SELECT head FROM logs_base) 
--     UNION ALL 
--     SELECT * FROM logs 
--     LIMIT (SELECT lmt FROM logs_base) ;
--   }
-- } {1 {no such table: logs_base}}


-- Overflow the lemon parser stack by providing an overly complex
-- expression.  Make sure that the overflow is detected and reported.
--
test:do_test(
    "misc5-7.1",
    function()
        test:execsql "CREATE TABLE t1(x  INT primary key)"
        sql = "INSERT INTO t1 VALUES("
        tail = ""
        for i = 0, 199, 1 do
            sql = sql .. "(1+"
            tail = tail .. ")"
        end
        sql = sql .. "2"..tail
        return test:catchsql(sql)
    end, {
        -- <misc5-7.1>
        1, "Failed to parse SQL statement: parser stack limit reached"
        -- </misc5-7.1>
    })

-- # Parser stack overflow is silently ignored when it occurs while parsing the
-- # schema.
-- #
-- do_test misc5-7.2 {
--   sql db2 :memory:
--   catchsql {
--     CREATE TABLE t1(x  INT UNIQUE);
--     UPDATE sql_master SET sql='CREATE table t(o CHECK(((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((;VALUES(o)';
--     BEGIN;
--     CREATE TABLE t2(y INT );
--     ROLLBACK;
--     DROP TABLE IF EXISTS D;
--   } db2
-- } {0 {}}
-- db2 close
-- # Ticket #1911
-- #
-- ifcapable compound {
--   do_test misc5-9.1 {
--     execsql {
--       SELECT name, type FROM sql_master WHERE name IS NULL
--       UNION
--       SELECT type, name FROM sql_master WHERE type IS NULL
--       ORDER BY 1, 2, 1, 2, 1, 2
--     }
--   } {}
--   do_test misc5-9.2 {
--     execsql {
--       SELECT name, type FROM sql_master WHERE name IS NULL
--       UNION
--       SELECT type, name FROM sql_master WHERE type IS NULL
--       ORDER BY 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2
--     }
--   } {}
-- }
-- Ticket #1912.  Make the tokenizer require a space after a numeric
-- literal.
--
test:do_catchsql_test(
    "misc5-10.1",
    [[
        SELECT 123abc
    ]], {
        -- <misc5-10.1>
        1, [[At line 1 at or near position 16: unrecognized token '123abc']]
        -- </misc5-10.1>
    })

test:do_catchsql_test(
    "misc5-10.2",
    [[
        SELECT 1*123.4e5ghi;
    ]], {
        -- <misc5-10.2>
        1, [[At line 1 at or near position 18: unrecognized token '123.4e5ghi']]
        -- </misc5-10.2>
    })

-- Additional integer encoding tests.
--
test:do_test(
    "misc5-11.1",
    function()
        return test:execsql [[
            CREATE TABLE t3(x  INT primary key);
            INSERT INTO t3 VALUES(-18);
            INSERT INTO t3 VALUES(-17);
            INSERT INTO t3 VALUES(-16);
            INSERT INTO t3 VALUES(-15);
            INSERT INTO t3 VALUES(-14);
            INSERT INTO t3 VALUES(-13);
            INSERT INTO t3 VALUES(-12);
            INSERT INTO t3 VALUES(-11);
            INSERT INTO t3 VALUES(-10);
            INSERT INTO t3 VALUES(-9);
            INSERT INTO t3 VALUES(-8);
            INSERT INTO t3 VALUES(-7);
            INSERT INTO t3 VALUES(-6);
            INSERT INTO t3 VALUES(-5);
            INSERT INTO t3 VALUES(-4);
            INSERT INTO t3 VALUES(-3);
            INSERT INTO t3 VALUES(-2);
            INSERT INTO t3 VALUES(-1);
            INSERT INTO t3 VALUES(0);
            INSERT INTO t3 VALUES(1);
            INSERT INTO t3 VALUES(2);
            INSERT INTO t3 VALUES(3);
            INSERT INTO t3 VALUES(4);
            INSERT INTO t3 VALUES(5);
            INSERT INTO t3 VALUES(6);
            INSERT INTO t3 VALUES(7);
            INSERT INTO t3 VALUES(8);
            INSERT INTO t3 VALUES(9);
            INSERT INTO t3 VALUES(10);
            INSERT INTO t3 VALUES(11);
            INSERT INTO t3 VALUES(12);
            INSERT INTO t3 VALUES(13);
            INSERT INTO t3 VALUES(14);
            INSERT INTO t3 VALUES(15);
            INSERT INTO t3 VALUES(16);
            INSERT INTO t3 VALUES(17);
            INSERT INTO t3 VALUES(18);
            INSERT INTO t3 VALUES(30);
            INSERT INTO t3 VALUES(31);
            INSERT INTO t3 VALUES(32);
            INSERT INTO t3 VALUES(33);
            INSERT INTO t3 VALUES(34);
            INSERT INTO t3 VALUES(-30);
            INSERT INTO t3 VALUES(-31);
            INSERT INTO t3 VALUES(-32);
            INSERT INTO t3 VALUES(-33);
            INSERT INTO t3 VALUES(-34);
            INSERT INTO t3 VALUES(62);
            INSERT INTO t3 VALUES(63);
            INSERT INTO t3 VALUES(64);
            INSERT INTO t3 VALUES(65);
            INSERT INTO t3 VALUES(66);
            INSERT INTO t3 VALUES(-62);
            INSERT INTO t3 VALUES(-63);
            INSERT INTO t3 VALUES(-64);
            INSERT INTO t3 VALUES(-65);
            INSERT INTO t3 VALUES(-66);
            INSERT INTO t3 VALUES(126);
            INSERT INTO t3 VALUES(127);
            INSERT INTO t3 VALUES(128);
            INSERT INTO t3 VALUES(129);
            INSERT INTO t3 VALUES(130);
            INSERT INTO t3 VALUES(-126);
            INSERT INTO t3 VALUES(-127);
            INSERT INTO t3 VALUES(-128);
            INSERT INTO t3 VALUES(-129);
            INSERT INTO t3 VALUES(-130);
            INSERT INTO t3 VALUES(254);
            INSERT INTO t3 VALUES(255);
            INSERT INTO t3 VALUES(256);
            INSERT INTO t3 VALUES(257);
            INSERT INTO t3 VALUES(258);
            INSERT INTO t3 VALUES(-254);
            INSERT INTO t3 VALUES(-255);
            INSERT INTO t3 VALUES(-256);
            INSERT INTO t3 VALUES(-257);
            INSERT INTO t3 VALUES(-258);
            INSERT INTO t3 VALUES(510);
            INSERT INTO t3 VALUES(511);
            INSERT INTO t3 VALUES(512);
            INSERT INTO t3 VALUES(513);
            INSERT INTO t3 VALUES(514);
            INSERT INTO t3 VALUES(-510);
            INSERT INTO t3 VALUES(-511);
            INSERT INTO t3 VALUES(-512);
            INSERT INTO t3 VALUES(-513);
            INSERT INTO t3 VALUES(-514);
            INSERT INTO t3 VALUES(1022);
            INSERT INTO t3 VALUES(1023);
            INSERT INTO t3 VALUES(1024);
            INSERT INTO t3 VALUES(1025);
            INSERT INTO t3 VALUES(1026);
            INSERT INTO t3 VALUES(-1022);
            INSERT INTO t3 VALUES(-1023);
            INSERT INTO t3 VALUES(-1024);
            INSERT INTO t3 VALUES(-1025);
            INSERT INTO t3 VALUES(-1026);
            INSERT INTO t3 VALUES(2046);
            INSERT INTO t3 VALUES(2047);
            INSERT INTO t3 VALUES(2048);
            INSERT INTO t3 VALUES(2049);
            INSERT INTO t3 VALUES(2050);
            INSERT INTO t3 VALUES(-2046);
            INSERT INTO t3 VALUES(-2047);
            INSERT INTO t3 VALUES(-2048);
            INSERT INTO t3 VALUES(-2049);
            INSERT INTO t3 VALUES(-2050);
            INSERT INTO t3 VALUES(4094);
            INSERT INTO t3 VALUES(4095);
            INSERT INTO t3 VALUES(4096);
            INSERT INTO t3 VALUES(4097);
            INSERT INTO t3 VALUES(4098);
            INSERT INTO t3 VALUES(-4094);
            INSERT INTO t3 VALUES(-4095);
            INSERT INTO t3 VALUES(-4096);
            INSERT INTO t3 VALUES(-4097);
            INSERT INTO t3 VALUES(-4098);
            INSERT INTO t3 VALUES(8190);
            INSERT INTO t3 VALUES(8191);
            INSERT INTO t3 VALUES(8192);
            INSERT INTO t3 VALUES(8193);
            INSERT INTO t3 VALUES(8194);
            INSERT INTO t3 VALUES(-8190);
            INSERT INTO t3 VALUES(-8191);
            INSERT INTO t3 VALUES(-8192);
            INSERT INTO t3 VALUES(-8193);
            INSERT INTO t3 VALUES(-8194);
            INSERT INTO t3 VALUES(16382);
            INSERT INTO t3 VALUES(16383);
            INSERT INTO t3 VALUES(16384);
            INSERT INTO t3 VALUES(16385);
            INSERT INTO t3 VALUES(16386);
            INSERT INTO t3 VALUES(-16382);
            INSERT INTO t3 VALUES(-16383);
            INSERT INTO t3 VALUES(-16384);
            INSERT INTO t3 VALUES(-16385);
            INSERT INTO t3 VALUES(-16386);
            INSERT INTO t3 VALUES(32766);
            INSERT INTO t3 VALUES(32767);
            INSERT INTO t3 VALUES(32768);
            INSERT INTO t3 VALUES(32769);
            INSERT INTO t3 VALUES(32770);
            INSERT INTO t3 VALUES(-32766);
            INSERT INTO t3 VALUES(-32767);
            INSERT INTO t3 VALUES(-32768);
            INSERT INTO t3 VALUES(-32769);
            INSERT INTO t3 VALUES(-32770);
            INSERT INTO t3 VALUES(65534);
            INSERT INTO t3 VALUES(65535);
            INSERT INTO t3 VALUES(65536);
            INSERT INTO t3 VALUES(65537);
            INSERT INTO t3 VALUES(65538);
            INSERT INTO t3 VALUES(-65534);
            INSERT INTO t3 VALUES(-65535);
            INSERT INTO t3 VALUES(-65536);
            INSERT INTO t3 VALUES(-65537);
            INSERT INTO t3 VALUES(-65538);
            INSERT INTO t3 VALUES(131070);
            INSERT INTO t3 VALUES(131071);
            INSERT INTO t3 VALUES(131072);
            INSERT INTO t3 VALUES(131073);
            INSERT INTO t3 VALUES(131074);
            INSERT INTO t3 VALUES(-131070);
            INSERT INTO t3 VALUES(-131071);
            INSERT INTO t3 VALUES(-131072);
            INSERT INTO t3 VALUES(-131073);
            INSERT INTO t3 VALUES(-131074);
            INSERT INTO t3 VALUES(262142);
            INSERT INTO t3 VALUES(262143);
            INSERT INTO t3 VALUES(262144);
            INSERT INTO t3 VALUES(262145);
            INSERT INTO t3 VALUES(262146);
            INSERT INTO t3 VALUES(-262142);
            INSERT INTO t3 VALUES(-262143);
            INSERT INTO t3 VALUES(-262144);
            INSERT INTO t3 VALUES(-262145);
            INSERT INTO t3 VALUES(-262146);
            INSERT INTO t3 VALUES(524286);
            INSERT INTO t3 VALUES(524287);
            INSERT INTO t3 VALUES(524288);
            INSERT INTO t3 VALUES(524289);
            INSERT INTO t3 VALUES(524290);
            INSERT INTO t3 VALUES(-524286);
            INSERT INTO t3 VALUES(-524287);
            INSERT INTO t3 VALUES(-524288);
            INSERT INTO t3 VALUES(-524289);
            INSERT INTO t3 VALUES(-524290);
            INSERT INTO t3 VALUES(1048574);
            INSERT INTO t3 VALUES(1048575);
            INSERT INTO t3 VALUES(1048576);
            INSERT INTO t3 VALUES(1048577);
            INSERT INTO t3 VALUES(1048578);
            INSERT INTO t3 VALUES(-1048574);
            INSERT INTO t3 VALUES(-1048575);
            INSERT INTO t3 VALUES(-1048576);
            INSERT INTO t3 VALUES(-1048577);
            INSERT INTO t3 VALUES(-1048578);
            INSERT INTO t3 VALUES(2097150);
            INSERT INTO t3 VALUES(2097151);
            INSERT INTO t3 VALUES(2097152);
            INSERT INTO t3 VALUES(2097153);
            INSERT INTO t3 VALUES(2097154);
            INSERT INTO t3 VALUES(-2097150);
            INSERT INTO t3 VALUES(-2097151);
            INSERT INTO t3 VALUES(-2097152);
            INSERT INTO t3 VALUES(-2097153);
            INSERT INTO t3 VALUES(-2097154);
            INSERT INTO t3 VALUES(4194302);
            INSERT INTO t3 VALUES(4194303);
            INSERT INTO t3 VALUES(4194304);
            INSERT INTO t3 VALUES(4194305);
            INSERT INTO t3 VALUES(4194306);
            INSERT INTO t3 VALUES(-4194302);
            INSERT INTO t3 VALUES(-4194303);
            INSERT INTO t3 VALUES(-4194304);
            INSERT INTO t3 VALUES(-4194305);
            INSERT INTO t3 VALUES(-4194306);
            INSERT INTO t3 VALUES(8388606);
            INSERT INTO t3 VALUES(8388607);
            INSERT INTO t3 VALUES(8388608);
            INSERT INTO t3 VALUES(8388609);
            INSERT INTO t3 VALUES(8388610);
            INSERT INTO t3 VALUES(-8388606);
            INSERT INTO t3 VALUES(-8388607);
            INSERT INTO t3 VALUES(-8388608);
            INSERT INTO t3 VALUES(-8388609);
            INSERT INTO t3 VALUES(-8388610);
            INSERT INTO t3 VALUES(16777214);
            INSERT INTO t3 VALUES(16777215);
            INSERT INTO t3 VALUES(16777216);
            INSERT INTO t3 VALUES(16777217);
            INSERT INTO t3 VALUES(16777218);
            INSERT INTO t3 VALUES(-16777214);
            INSERT INTO t3 VALUES(-16777215);
            INSERT INTO t3 VALUES(-16777216);
            INSERT INTO t3 VALUES(-16777217);
            INSERT INTO t3 VALUES(-16777218);
            INSERT INTO t3 VALUES(33554430);
            INSERT INTO t3 VALUES(33554431);
            INSERT INTO t3 VALUES(33554432);
            INSERT INTO t3 VALUES(33554433);
            INSERT INTO t3 VALUES(33554434);
            INSERT INTO t3 VALUES(-33554430);
            INSERT INTO t3 VALUES(-33554431);
            INSERT INTO t3 VALUES(-33554432);
            INSERT INTO t3 VALUES(-33554433);
            INSERT INTO t3 VALUES(-33554434);
            INSERT INTO t3 VALUES(67108862);
            INSERT INTO t3 VALUES(67108863);
            INSERT INTO t3 VALUES(67108864);
            INSERT INTO t3 VALUES(67108865);
            INSERT INTO t3 VALUES(67108866);
            INSERT INTO t3 VALUES(-67108862);
            INSERT INTO t3 VALUES(-67108863);
            INSERT INTO t3 VALUES(-67108864);
            INSERT INTO t3 VALUES(-67108865);
            INSERT INTO t3 VALUES(-67108866);
            INSERT INTO t3 VALUES(134217726);
            INSERT INTO t3 VALUES(134217727);
            INSERT INTO t3 VALUES(134217728);
            INSERT INTO t3 VALUES(134217729);
            INSERT INTO t3 VALUES(134217730);
            INSERT INTO t3 VALUES(-134217726);
            INSERT INTO t3 VALUES(-134217727);
            INSERT INTO t3 VALUES(-134217728);
            INSERT INTO t3 VALUES(-134217729);
            INSERT INTO t3 VALUES(-134217730);
            INSERT INTO t3 VALUES(268435454);
            INSERT INTO t3 VALUES(268435455);
            INSERT INTO t3 VALUES(268435456);
            INSERT INTO t3 VALUES(268435457);
            INSERT INTO t3 VALUES(268435458);
            INSERT INTO t3 VALUES(-268435454);
            INSERT INTO t3 VALUES(-268435455);
            INSERT INTO t3 VALUES(-268435456);
            INSERT INTO t3 VALUES(-268435457);
            INSERT INTO t3 VALUES(-268435458);
            INSERT INTO t3 VALUES(536870910);
            INSERT INTO t3 VALUES(536870911);
            INSERT INTO t3 VALUES(536870912);
            INSERT INTO t3 VALUES(536870913);
            INSERT INTO t3 VALUES(536870914);
            INSERT INTO t3 VALUES(-536870910);
            INSERT INTO t3 VALUES(-536870911);
            INSERT INTO t3 VALUES(-536870912);
            INSERT INTO t3 VALUES(-536870913);
            INSERT INTO t3 VALUES(-536870914);
            INSERT INTO t3 VALUES(1073741822);
            INSERT INTO t3 VALUES(1073741823);
            INSERT INTO t3 VALUES(1073741824);
            INSERT INTO t3 VALUES(1073741825);
            INSERT INTO t3 VALUES(1073741826);
            INSERT INTO t3 VALUES(-1073741822);
            INSERT INTO t3 VALUES(-1073741823);
            INSERT INTO t3 VALUES(-1073741824);
            INSERT INTO t3 VALUES(-1073741825);
            INSERT INTO t3 VALUES(-1073741826);
            INSERT INTO t3 VALUES(2147483646);
            INSERT INTO t3 VALUES(2147483647);
            INSERT INTO t3 VALUES(2147483648);
            INSERT INTO t3 VALUES(2147483649);
            INSERT INTO t3 VALUES(2147483650);
            INSERT INTO t3 VALUES(-2147483646);
            INSERT INTO t3 VALUES(-2147483647);
            INSERT INTO t3 VALUES(-2147483648);
            INSERT INTO t3 VALUES(-2147483649);
            INSERT INTO t3 VALUES(-2147483650);
            INSERT INTO t3 VALUES(4294967294);
            INSERT INTO t3 VALUES(4294967295);
            INSERT INTO t3 VALUES(4294967296);
            INSERT INTO t3 VALUES(4294967297);
            INSERT INTO t3 VALUES(4294967298);
            INSERT INTO t3 VALUES(-4294967294);
            INSERT INTO t3 VALUES(-4294967295);
            INSERT INTO t3 VALUES(-4294967296);
            INSERT INTO t3 VALUES(-4294967297);
            INSERT INTO t3 VALUES(-4294967298);
            INSERT INTO t3 VALUES(8589934590);
            INSERT INTO t3 VALUES(8589934591);
            INSERT INTO t3 VALUES(8589934592);
            INSERT INTO t3 VALUES(8589934593);
            INSERT INTO t3 VALUES(8589934594);
            INSERT INTO t3 VALUES(-8589934590);
            INSERT INTO t3 VALUES(-8589934591);
            INSERT INTO t3 VALUES(-8589934592);
            INSERT INTO t3 VALUES(-8589934593);
            INSERT INTO t3 VALUES(-8589934594);
            INSERT INTO t3 VALUES(17179869182);
            INSERT INTO t3 VALUES(17179869183);
            INSERT INTO t3 VALUES(17179869184);
            INSERT INTO t3 VALUES(17179869185);
            INSERT INTO t3 VALUES(17179869186);
            INSERT INTO t3 VALUES(-17179869182);
            INSERT INTO t3 VALUES(-17179869183);
            INSERT INTO t3 VALUES(-17179869184);
            INSERT INTO t3 VALUES(-17179869185);
            INSERT INTO t3 VALUES(-17179869186);
            INSERT INTO t3 VALUES(34359738366);
            INSERT INTO t3 VALUES(34359738367);
            INSERT INTO t3 VALUES(34359738368);
            INSERT INTO t3 VALUES(34359738369);
            INSERT INTO t3 VALUES(34359738370);
            INSERT INTO t3 VALUES(-34359738366);
            INSERT INTO t3 VALUES(-34359738367);
            INSERT INTO t3 VALUES(-34359738368);
            INSERT INTO t3 VALUES(-34359738369);
            INSERT INTO t3 VALUES(-34359738370);
            INSERT INTO t3 VALUES(68719476734);
            INSERT INTO t3 VALUES(68719476735);
            INSERT INTO t3 VALUES(68719476736);
            INSERT INTO t3 VALUES(68719476737);
            INSERT INTO t3 VALUES(68719476738);
            INSERT INTO t3 VALUES(-68719476734);
            INSERT INTO t3 VALUES(-68719476735);
            INSERT INTO t3 VALUES(-68719476736);
            INSERT INTO t3 VALUES(-68719476737);
            INSERT INTO t3 VALUES(-68719476738);
            INSERT INTO t3 VALUES(137438953470);
            INSERT INTO t3 VALUES(137438953471);
            INSERT INTO t3 VALUES(137438953472);
            INSERT INTO t3 VALUES(137438953473);
            INSERT INTO t3 VALUES(137438953474);
            INSERT INTO t3 VALUES(-137438953470);
            INSERT INTO t3 VALUES(-137438953471);
            INSERT INTO t3 VALUES(-137438953472);
            INSERT INTO t3 VALUES(-137438953473);
            INSERT INTO t3 VALUES(-137438953474);
            INSERT INTO t3 VALUES(274877906942);
            INSERT INTO t3 VALUES(274877906943);
            INSERT INTO t3 VALUES(274877906944);
            INSERT INTO t3 VALUES(274877906945);
            INSERT INTO t3 VALUES(274877906946);
            INSERT INTO t3 VALUES(-274877906942);
            INSERT INTO t3 VALUES(-274877906943);
            INSERT INTO t3 VALUES(-274877906944);
            INSERT INTO t3 VALUES(-274877906945);
            INSERT INTO t3 VALUES(-274877906946);
            INSERT INTO t3 VALUES(549755813886);
            INSERT INTO t3 VALUES(549755813887);
            INSERT INTO t3 VALUES(549755813888);
            INSERT INTO t3 VALUES(549755813889);
            INSERT INTO t3 VALUES(549755813890);
            INSERT INTO t3 VALUES(-549755813886);
            INSERT INTO t3 VALUES(-549755813887);
            INSERT INTO t3 VALUES(-549755813888);
            INSERT INTO t3 VALUES(-549755813889);
            INSERT INTO t3 VALUES(-549755813890);
            INSERT INTO t3 VALUES(1099511627774);
            INSERT INTO t3 VALUES(1099511627775);
            INSERT INTO t3 VALUES(1099511627776);
            INSERT INTO t3 VALUES(1099511627777);
            INSERT INTO t3 VALUES(1099511627778);
            INSERT INTO t3 VALUES(-1099511627774);
            INSERT INTO t3 VALUES(-1099511627775);
            INSERT INTO t3 VALUES(-1099511627776);
            INSERT INTO t3 VALUES(-1099511627777);
            INSERT INTO t3 VALUES(-1099511627778);
            INSERT INTO t3 VALUES(2199023255550);
            INSERT INTO t3 VALUES(2199023255551);
            INSERT INTO t3 VALUES(2199023255552);
            INSERT INTO t3 VALUES(2199023255553);
            INSERT INTO t3 VALUES(2199023255554);
            INSERT INTO t3 VALUES(-2199023255550);
            INSERT INTO t3 VALUES(-2199023255551);
            INSERT INTO t3 VALUES(-2199023255552);
            INSERT INTO t3 VALUES(-2199023255553);
            INSERT INTO t3 VALUES(-2199023255554);
            INSERT INTO t3 VALUES(4398046511102);
            INSERT INTO t3 VALUES(4398046511103);
            INSERT INTO t3 VALUES(4398046511104);
            INSERT INTO t3 VALUES(4398046511105);
            INSERT INTO t3 VALUES(4398046511106);
            INSERT INTO t3 VALUES(-4398046511102);
            INSERT INTO t3 VALUES(-4398046511103);
            INSERT INTO t3 VALUES(-4398046511104);
            INSERT INTO t3 VALUES(-4398046511105);
            INSERT INTO t3 VALUES(-4398046511106);
            INSERT INTO t3 VALUES(8796093022206);
            INSERT INTO t3 VALUES(8796093022207);
            INSERT INTO t3 VALUES(8796093022208);
            INSERT INTO t3 VALUES(8796093022209);
            INSERT INTO t3 VALUES(8796093022210);
            INSERT INTO t3 VALUES(-8796093022206);
            INSERT INTO t3 VALUES(-8796093022207);
            INSERT INTO t3 VALUES(-8796093022208);
            INSERT INTO t3 VALUES(-8796093022209);
            INSERT INTO t3 VALUES(-8796093022210);
            INSERT INTO t3 VALUES(17592186044414);
            INSERT INTO t3 VALUES(17592186044415);
            INSERT INTO t3 VALUES(17592186044416);
            INSERT INTO t3 VALUES(17592186044417);
            INSERT INTO t3 VALUES(17592186044418);
            INSERT INTO t3 VALUES(-17592186044414);
            INSERT INTO t3 VALUES(-17592186044415);
            INSERT INTO t3 VALUES(-17592186044416);
            INSERT INTO t3 VALUES(-17592186044417);
            INSERT INTO t3 VALUES(-17592186044418);
            INSERT INTO t3 VALUES(35184372088830);
            INSERT INTO t3 VALUES(35184372088831);
            INSERT INTO t3 VALUES(35184372088832);
            INSERT INTO t3 VALUES(35184372088833);
            INSERT INTO t3 VALUES(35184372088834);
            INSERT INTO t3 VALUES(-35184372088830);
            INSERT INTO t3 VALUES(-35184372088831);
            INSERT INTO t3 VALUES(-35184372088832);
            INSERT INTO t3 VALUES(-35184372088833);
            INSERT INTO t3 VALUES(-35184372088834);
            INSERT INTO t3 VALUES(70368744177662);
            INSERT INTO t3 VALUES(70368744177663);
            INSERT INTO t3 VALUES(70368744177664);
            INSERT INTO t3 VALUES(70368744177665);
            INSERT INTO t3 VALUES(70368744177666);
            INSERT INTO t3 VALUES(-70368744177662);
            INSERT INTO t3 VALUES(-70368744177663);
            INSERT INTO t3 VALUES(-70368744177664);
            INSERT INTO t3 VALUES(-70368744177665);
            INSERT INTO t3 VALUES(-70368744177666);
            INSERT INTO t3 VALUES(140737488355326);
            INSERT INTO t3 VALUES(140737488355327);
            INSERT INTO t3 VALUES(140737488355328);
            INSERT INTO t3 VALUES(140737488355329);
            INSERT INTO t3 VALUES(140737488355330);
            INSERT INTO t3 VALUES(-140737488355326);
            INSERT INTO t3 VALUES(-140737488355327);
            INSERT INTO t3 VALUES(-140737488355328);
            INSERT INTO t3 VALUES(-140737488355329);
            INSERT INTO t3 VALUES(-140737488355330);
            INSERT INTO t3 VALUES(281474976710654);
            INSERT INTO t3 VALUES(281474976710655);
            INSERT INTO t3 VALUES(281474976710656);
            INSERT INTO t3 VALUES(281474976710657);
            INSERT INTO t3 VALUES(281474976710658);
            INSERT INTO t3 VALUES(-281474976710654);
            INSERT INTO t3 VALUES(-281474976710655);
            INSERT INTO t3 VALUES(-281474976710656);
            INSERT INTO t3 VALUES(-281474976710657);
            INSERT INTO t3 VALUES(-281474976710658);
            INSERT INTO t3 VALUES(562949953421310);
            INSERT INTO t3 VALUES(562949953421311);
            INSERT INTO t3 VALUES(562949953421312);
            INSERT INTO t3 VALUES(562949953421313);
            INSERT INTO t3 VALUES(562949953421314);
            INSERT INTO t3 VALUES(-562949953421310);
            INSERT INTO t3 VALUES(-562949953421311);
            INSERT INTO t3 VALUES(-562949953421312);
            INSERT INTO t3 VALUES(-562949953421313);
            INSERT INTO t3 VALUES(-562949953421314);
            INSERT INTO t3 VALUES(1125899906842622);
            INSERT INTO t3 VALUES(1125899906842623);
            INSERT INTO t3 VALUES(1125899906842624);
            INSERT INTO t3 VALUES(1125899906842625);
            INSERT INTO t3 VALUES(1125899906842626);
            INSERT INTO t3 VALUES(-1125899906842622);
            INSERT INTO t3 VALUES(-1125899906842623);
            INSERT INTO t3 VALUES(-1125899906842624);
            INSERT INTO t3 VALUES(-1125899906842625);
            INSERT INTO t3 VALUES(-1125899906842626);
            INSERT INTO t3 VALUES(2251799813685246);
            INSERT INTO t3 VALUES(2251799813685247);
            INSERT INTO t3 VALUES(2251799813685248);
            INSERT INTO t3 VALUES(2251799813685249);
            INSERT INTO t3 VALUES(2251799813685250);
            INSERT INTO t3 VALUES(-2251799813685246);
            INSERT INTO t3 VALUES(-2251799813685247);
            INSERT INTO t3 VALUES(-2251799813685248);
            INSERT INTO t3 VALUES(-2251799813685249);
            INSERT INTO t3 VALUES(-2251799813685250);
            INSERT INTO t3 VALUES(4503599627370494);
            INSERT INTO t3 VALUES(4503599627370495);
            INSERT INTO t3 VALUES(4503599627370496);
            INSERT INTO t3 VALUES(4503599627370497);
            INSERT INTO t3 VALUES(4503599627370498);
            INSERT INTO t3 VALUES(-4503599627370494);
            INSERT INTO t3 VALUES(-4503599627370495);
            INSERT INTO t3 VALUES(-4503599627370496);
            INSERT INTO t3 VALUES(-4503599627370497);
            INSERT INTO t3 VALUES(-4503599627370498);
            INSERT INTO t3 VALUES(9007199254740990);
            INSERT INTO t3 VALUES(9007199254740991);
            INSERT INTO t3 VALUES(9007199254740992);
            INSERT INTO t3 VALUES(9007199254740993);
            INSERT INTO t3 VALUES(9007199254740994);
            INSERT INTO t3 VALUES(-9007199254740990);
            INSERT INTO t3 VALUES(-9007199254740991);
            INSERT INTO t3 VALUES(-9007199254740992);
            INSERT INTO t3 VALUES(-9007199254740993);
            INSERT INTO t3 VALUES(-9007199254740994);
            INSERT INTO t3 VALUES(18014398509481982);
            INSERT INTO t3 VALUES(18014398509481983);
            INSERT INTO t3 VALUES(18014398509481984);
            INSERT INTO t3 VALUES(18014398509481985);
            INSERT INTO t3 VALUES(18014398509481986);
            INSERT INTO t3 VALUES(-18014398509481982);
            INSERT INTO t3 VALUES(-18014398509481983);
            INSERT INTO t3 VALUES(-18014398509481984);
            INSERT INTO t3 VALUES(-18014398509481985);
            INSERT INTO t3 VALUES(-18014398509481986);
            INSERT INTO t3 VALUES(36028797018963966);
            INSERT INTO t3 VALUES(36028797018963967);
            INSERT INTO t3 VALUES(36028797018963968);
            INSERT INTO t3 VALUES(36028797018963969);
            INSERT INTO t3 VALUES(36028797018963970);
            INSERT INTO t3 VALUES(-36028797018963966);
            INSERT INTO t3 VALUES(-36028797018963967);
            INSERT INTO t3 VALUES(-36028797018963968);
            INSERT INTO t3 VALUES(-36028797018963969);
            INSERT INTO t3 VALUES(-36028797018963970);
            INSERT INTO t3 VALUES(72057594037927934);
            INSERT INTO t3 VALUES(72057594037927935);
            INSERT INTO t3 VALUES(72057594037927936);
            INSERT INTO t3 VALUES(72057594037927937);
            INSERT INTO t3 VALUES(72057594037927938);
            INSERT INTO t3 VALUES(-72057594037927934);
            INSERT INTO t3 VALUES(-72057594037927935);
            INSERT INTO t3 VALUES(-72057594037927936);
            INSERT INTO t3 VALUES(-72057594037927937);
            INSERT INTO t3 VALUES(-72057594037927938);
            INSERT INTO t3 VALUES(144115188075855870);
            INSERT INTO t3 VALUES(144115188075855871);
            INSERT INTO t3 VALUES(144115188075855872);
            INSERT INTO t3 VALUES(144115188075855873);
            INSERT INTO t3 VALUES(144115188075855874);
            INSERT INTO t3 VALUES(-144115188075855870);
            INSERT INTO t3 VALUES(-144115188075855871);
            INSERT INTO t3 VALUES(-144115188075855872);
            INSERT INTO t3 VALUES(-144115188075855873);
            INSERT INTO t3 VALUES(-144115188075855874);
            INSERT INTO t3 VALUES(288230376151711742);
            INSERT INTO t3 VALUES(288230376151711743);
            INSERT INTO t3 VALUES(288230376151711744);
            INSERT INTO t3 VALUES(288230376151711745);
            INSERT INTO t3 VALUES(288230376151711746);
            INSERT INTO t3 VALUES(-288230376151711742);
            INSERT INTO t3 VALUES(-288230376151711743);
            INSERT INTO t3 VALUES(-288230376151711744);
            INSERT INTO t3 VALUES(-288230376151711745);
            INSERT INTO t3 VALUES(-288230376151711746);
            INSERT INTO t3 VALUES(576460752303423486);
            INSERT INTO t3 VALUES(576460752303423487);
            INSERT INTO t3 VALUES(576460752303423488);
            INSERT INTO t3 VALUES(576460752303423489);
            INSERT INTO t3 VALUES(576460752303423490);
            INSERT INTO t3 VALUES(-576460752303423486);
            INSERT INTO t3 VALUES(-576460752303423487);
            INSERT INTO t3 VALUES(-576460752303423488);
            INSERT INTO t3 VALUES(-576460752303423489);
            INSERT INTO t3 VALUES(-576460752303423490);
            INSERT INTO t3 VALUES(1152921504606846974);
            INSERT INTO t3 VALUES(1152921504606846975);
            INSERT INTO t3 VALUES(1152921504606846976);
            INSERT INTO t3 VALUES(1152921504606846977);
            INSERT INTO t3 VALUES(1152921504606846978);
            INSERT INTO t3 VALUES(-1152921504606846974);
            INSERT INTO t3 VALUES(-1152921504606846975);
            INSERT INTO t3 VALUES(-1152921504606846976);
            INSERT INTO t3 VALUES(-1152921504606846977);
            INSERT INTO t3 VALUES(-1152921504606846978);
            INSERT INTO t3 VALUES(2305843009213693950);
            INSERT INTO t3 VALUES(2305843009213693951);
            INSERT INTO t3 VALUES(2305843009213693952);
            INSERT INTO t3 VALUES(2305843009213693953);
            INSERT INTO t3 VALUES(2305843009213693954);
            INSERT INTO t3 VALUES(-2305843009213693950);
            INSERT INTO t3 VALUES(-2305843009213693951);
            INSERT INTO t3 VALUES(-2305843009213693952);
            INSERT INTO t3 VALUES(-2305843009213693953);
            INSERT INTO t3 VALUES(-2305843009213693954);
            INSERT INTO t3 VALUES(4611686018427387902);
            INSERT INTO t3 VALUES(4611686018427387903);
            INSERT INTO t3 VALUES(4611686018427387904);
            INSERT INTO t3 VALUES(4611686018427387905);
            INSERT INTO t3 VALUES(4611686018427387906);
            INSERT INTO t3 VALUES(-4611686018427387902);
            INSERT INTO t3 VALUES(-4611686018427387903);
            INSERT INTO t3 VALUES(-4611686018427387904);
            INSERT INTO t3 VALUES(-4611686018427387905);
            INSERT INTO t3 VALUES(-4611686018427387906);
            INSERT INTO t3 VALUES(9223372036854775806);
            INSERT INTO t3 VALUES(9223372036854775807);
            INSERT INTO t3 VALUES(-9223372036854775806);
            INSERT INTO t3 VALUES(-9223372036854775807);
            INSERT INTO t3 VALUES(-9223372036854775808);
            SELECT x FROM t3 ORDER BY x;
        ]]
    end, {
        -- <misc5-11.1> 
    -9223372036854775808LL, -9223372036854775807LL, -9223372036854775806LL, -4611686018427387906LL,
 -4611686018427387905LL, -4611686018427387904LL, -4611686018427387903LL, -4611686018427387902LL,
 -2305843009213693954LL, -2305843009213693953LL, -2305843009213693952LL, -2305843009213693951LL,
 -2305843009213693950LL, -1152921504606846978LL, -1152921504606846977LL, -1152921504606846976LL,
 -1152921504606846975LL, -1152921504606846974LL, -576460752303423490LL, -576460752303423489LL,
 -576460752303423488LL, -576460752303423487LL, -576460752303423486LL, -288230376151711746LL,
 -288230376151711745LL, -288230376151711744LL, -288230376151711743LL, -288230376151711742LL,
 -144115188075855874LL, -144115188075855873LL, -144115188075855872LL, -144115188075855871LL,
 -144115188075855870LL, -72057594037927938LL, -72057594037927937LL, -72057594037927936LL,
 -72057594037927935LL, -72057594037927934LL, -36028797018963970LL, -36028797018963969LL,
 -36028797018963968LL, -36028797018963967LL, -36028797018963966LL, -18014398509481986LL,
 -18014398509481985LL, -18014398509481984LL, -18014398509481983LL, -18014398509481982LL,
 -9007199254740994LL, -9007199254740993LL, -9007199254740992LL, -9007199254740991LL,
 -9007199254740990LL, -4503599627370498LL, -4503599627370497LL, -4503599627370496LL,
 -4503599627370495LL, -4503599627370494LL, -2251799813685250LL, -2251799813685249LL,
 -2251799813685248LL, -2251799813685247LL, -2251799813685246LL, -1125899906842626LL,
 -1125899906842625LL, -1125899906842624LL, -1125899906842623LL, -1125899906842622LL, -562949953421314LL,
 -562949953421313LL, -562949953421312LL, -562949953421311LL, -562949953421310LL, -281474976710658LL,
 -281474976710657LL, -281474976710656LL, -281474976710655LL, -281474976710654LL, -140737488355330LL,
 -140737488355329LL, -140737488355328LL, -140737488355327LL, -140737488355326LL, -70368744177666,
 -70368744177665, -70368744177664, -70368744177663, -70368744177662, -35184372088834, -35184372088833,
 -35184372088832, -35184372088831, -35184372088830, -17592186044418, -17592186044417, -17592186044416,
 -17592186044415, -17592186044414, -8796093022210, -8796093022209, -8796093022208, -8796093022207, -8796093022206,
 -4398046511106, -4398046511105, -4398046511104, -4398046511103, -4398046511102, -2199023255554, -2199023255553,
 -2199023255552, -2199023255551, -2199023255550, -1099511627778, -1099511627777, -1099511627776, -1099511627775,
 -1099511627774, -549755813890, -549755813889, -549755813888, -549755813887, -549755813886, -274877906946,
 -274877906945, -274877906944, -274877906943, -274877906942, -137438953474, -137438953473, -137438953472,
 -137438953471, -137438953470, -68719476738, -68719476737, -68719476736,
 -68719476735, -68719476734, -34359738370, -34359738369, -34359738368, -34359738367, -34359738366, -17179869186,
 -17179869185, -17179869184, -17179869183, -17179869182, -8589934594, -8589934593, -8589934592, -8589934591,
 -8589934590, -4294967298, -4294967297, -4294967296, -4294967295, -4294967294, -2147483650, -2147483649,
 -2147483648, -2147483647, -2147483646, -1073741826, -1073741825, -1073741824, -1073741823, -1073741822,
 -536870914, -536870913, -536870912, -536870911, -536870910, -268435458, -268435457, -268435456,
 -268435455, -268435454, -134217730, -134217729, -134217728, -134217727, -134217726, -67108866,
 -67108865, -67108864, -67108863, -67108862, -33554434, -33554433, -33554432, -33554431,
 -33554430, -16777218, -16777217, -16777216, -16777215, -16777214, -8388610, -8388609,
 -8388608, -8388607, -8388606, -4194306, -4194305, -4194304, -4194303, -4194302,
 -2097154, -2097153, -2097152, -2097151, -2097150, -1048578, -1048577, -1048576,
 -1048575, -1048574, -524290, -524289, -524288, -524287, -524286, -262146,
 -262145, -262144, -262143, -262142, -131074, -131073, -131072, -131071, -131070, -65538, -65537, -65536,
 -65535, -65534, -32770, -32769, -32768, -32767, -32766, -16386, -16385, -16384, -16383, -16382,
 -8194, -8193, -8192, -8191, -8190, -4098, -4097, -4096, -4095, -4094, -2050, -2049,
 -2048, -2047, -2046, -1026, -1025, -1024, -1023, -1022, -514, -513, -512, -511,
 -510, -258, -257, -256, -255, -254, -130, -129, -128, -127, -126, -66, -65, -64, -63, -62,
 -34, -33, -32, -31, -30, -18, -17, -16, -15, -14, -13, -12, -11, -10, -9, -8, -7, -6, -5, -4,
 -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 30, 31, 32, 33, 34, 62,
 63, 64, 65, 66, 126, 127, 128, 129, 130, 254, 255, 256, 257, 258, 510, 511, 512, 513, 514, 1022,
 1023, 1024, 1025, 1026, 2046, 2047, 2048, 2049, 2050, 4094, 4095, 4096, 4097, 4098, 8190, 8191, 8192, 8193, 8194,
 16382, 16383, 16384, 16385, 16386, 32766, 32767, 32768, 32769, 32770, 65534, 65535, 65536, 65537, 65538, 131070,
 131071, 131072, 131073, 131074, 262142, 262143, 262144, 262145, 262146, 524286, 524287, 524288, 524289, 524290,
 1048574, 1048575, 1048576, 1048577, 1048578, 2097150, 2097151, 2097152, 2097153, 2097154, 4194302, 4194303, 4194304,
 4194305, 4194306, 8388606, 8388607, 8388608, 8388609, 8388610, 16777214, 16777215, 16777216, 16777217, 16777218,
 33554430, 33554431, 33554432, 33554433, 33554434, 67108862, 67108863, 67108864, 67108865, 67108866, 134217726,
 134217727, 134217728, 134217729, 134217730, 268435454, 268435455, 268435456, 268435457, 268435458, 536870910,
 536870911, 536870912, 536870913, 536870914, 1073741822, 1073741823, 1073741824, 1073741825, 1073741826, 2147483646,
 2147483647, 2147483648, 2147483649, 2147483650, 4294967294, 4294967295, 4294967296, 4294967297, 4294967298, 8589934590,
 8589934591, 8589934592, 8589934593, 8589934594, 17179869182, 17179869183, 17179869184, 17179869185, 17179869186,
 34359738366, 34359738367, 34359738368, 34359738369, 34359738370, 68719476734, 68719476735, 68719476736,
 68719476737, 68719476738, 137438953470, 137438953471, 137438953472, 137438953473, 137438953474, 274877906942,
 274877906943, 274877906944, 274877906945, 274877906946, 549755813886, 549755813887, 549755813888, 549755813889,
 549755813890, 1099511627774, 1099511627775, 1099511627776, 1099511627777, 1099511627778, 2199023255550, 2199023255551,
 2199023255552, 2199023255553, 2199023255554, 4398046511102, 4398046511103, 4398046511104, 4398046511105, 4398046511106,
 8796093022206, 8796093022207, 8796093022208, 8796093022209, 8796093022210, 17592186044414, 17592186044415, 17592186044416,
 17592186044417, 17592186044418, 35184372088830, 35184372088831, 35184372088832, 35184372088833, 35184372088834,
 70368744177662, 70368744177663, 70368744177664, 70368744177665, 70368744177666, 140737488355326LL, 140737488355327LL,
 140737488355328LL, 140737488355329LL, 140737488355330LL, 281474976710654LL, 281474976710655LL, 281474976710656LL,
 281474976710657LL, 281474976710658LL, 562949953421310LL, 562949953421311LL,
 562949953421312LL, 562949953421313LL, 562949953421314LL, 1125899906842622LL,
 1125899906842623LL, 1125899906842624LL, 1125899906842625LL, 1125899906842626LL,
 2251799813685246LL, 2251799813685247LL, 2251799813685248LL, 2251799813685249LL,
 2251799813685250LL, 4503599627370494LL, 4503599627370495LL, 4503599627370496LL,
 4503599627370497LL, 4503599627370498LL, 9007199254740990LL, 9007199254740991LL,
 9007199254740992LL, 9007199254740993LL, 9007199254740994LL, 18014398509481982LL,
 18014398509481983LL, 18014398509481984LL, 18014398509481985LL, 18014398509481986LL,
 36028797018963966LL, 36028797018963967LL, 36028797018963968LL, 36028797018963969LL,
 36028797018963970LL, 72057594037927934LL, 72057594037927935LL, 72057594037927936LL,
 72057594037927937LL, 72057594037927938LL, 144115188075855870LL, 144115188075855871LL,
 144115188075855872LL, 144115188075855873LL, 144115188075855874LL, 288230376151711742LL,
 288230376151711743LL, 288230376151711744LL, 288230376151711745LL, 288230376151711746LL,
 576460752303423486LL, 576460752303423487LL, 576460752303423488LL, 576460752303423489LL,
 576460752303423490LL, 1152921504606846974LL, 1152921504606846975LL, 1152921504606846976LL,
 1152921504606846977LL, 1152921504606846978LL, 2305843009213693950LL, 2305843009213693951LL,
 2305843009213693952LL, 2305843009213693953LL, 2305843009213693954LL, 4611686018427387902LL,
 4611686018427387903LL, 4611686018427387904LL, 4611686018427387905LL, 4611686018427387906LL,
 9223372036854775806LL, 9223372036854775807LL,
        -- </misc5-11.1>
    })



test:finish_test()
