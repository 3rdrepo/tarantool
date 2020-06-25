#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(32)

--
-- Make sure that there is no implicit cast between string and
-- number.
--
test:do_catchsql_test(
    "gh-4230-1",
    [[
        SELECT '1' > 0;
    ]], {
        1, "Type mismatch: can not convert 1 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-2",
    [[
        SELECT 0 > '1';
    ]], {
        1, "Type mismatch: can not convert 1 to numeric"
    })

test:execsql([[
        CREATE TABLE t (i INT PRIMARY KEY, d DOUBLE, n NUMBER, s STRING);
        INSERT INTO t VALUES (1, 1.0, 1, '2'), (2, 2.0, 2.0, '2');
    ]])

test:do_catchsql_test(
    "gh-4230-3",
    [[
        SELECT * from t where i > s;
    ]], {
        1, "Type mismatch: can not convert 2 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-4",
    [[
        SELECT * from t WHERE s > i;
    ]], {
        1, "Type mismatch: can not convert 2 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-5",
    [[
        SELECT * from t WHERE d > s;
    ]], {
        1, "Type mismatch: can not convert 2 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-6",
    [[
        SELECT * from t WHERE s > d;
    ]], {
        1, "Type mismatch: can not convert 2 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-7",
    [[
        SELECT * from t WHERE i = 1 and n > s;
    ]], {
        1, "Type mismatch: can not convert 2 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-8",
    [[
        SELECT * from t WHERE i = 2 and s > n;
    ]], {
        1, "Type mismatch: can not convert 2 to numeric"
    })

test:execsql([[
    CREATE TABLE t1(x TEXT primary key);
    INSERT INTO t1 VALUES('1');
    CREATE TABLE t2(x INTEGER primary key);
    INSERT INTO t2 VALUES(1);
    CREATE TABLE t3(x DOUBLE primary key);
    INSERT INTO t3 VALUES(1.0);
]])

test:do_catchsql_test(
    "gh-4230-9",
    [[
        SELECT x FROM t1 WHERE x IN (1);
    ]], {
        1, "Type mismatch: can not convert 1 to numeric"
    })


test:do_catchsql_test(
    "gh-4230-10",
    [[
        SELECT x FROM t1 WHERE x IN (1.0);
    ]], {
        1, "Type mismatch: can not convert 1 to numeric"
    })

test:do_execsql_test(
    "gh-4230-11",
    [[
        SELECT x FROM t1 WHERE x IN ('1');
    ]], {
        "1"
    })

test:do_execsql_test(
    "gh-4230-12",
    [[
        SELECT x FROM t1 WHERE x IN ('1.0');
    ]], {
    })

test:do_catchsql_test(
    "gh-4230-13",
    [[
        SELECT x FROM t1 WHERE 1 IN (x);
    ]], {
        1, "Type mismatch: can not convert 1 to numeric"
    })

test:do_catchsql_test(
    "gh-4230-14",
    [[
        SELECT x FROM t1 WHERE 1.0 IN (x);
    ]], {
        1, "Type mismatch: can not convert 1 to numeric"
    })

test:do_execsql_test(
    "gh-4230-15",
    [[
        SELECT x FROM t1 WHERE '1' IN (x);
    ]], {
        -- <2.7>
        "1"
        -- </2.7>
    })

test:do_execsql_test(
    "gh-4230-16",
    [[
        SELECT x FROM t1 WHERE '1.0' IN (x);
    ]], {
    })

test:do_execsql_test(
    "gh-4230-17",
    [[
        SELECT x FROM t2 WHERE x IN (1);
    ]], {
        1
    })


test:do_execsql_test(
    "gh-4230-18",
    [[
        SELECT x FROM t2 WHERE x IN (1.0);
    ]], {
        1
    })

test:do_catchsql_test(
    "gh-4230-19",
    [[
        SELECT x FROM t2 WHERE x IN ('1');
    ]], {
        1, "Type mismatch: can not convert integer to text"
    })

test:do_catchsql_test(
    "gh-4230-20",
    [[
        SELECT x FROM t2 WHERE x IN ('1.0');
    ]], {
        1, "Type mismatch: can not convert integer to text"
    })

test:do_execsql_test(
    "gh-4230-21",
    [[
        SELECT x FROM t2 WHERE 1 IN (x);
    ]], {
        1
    })

test:do_execsql_test(
    "gh-4230-22",
    [[
        SELECT x FROM t2 WHERE 1.0 IN (x);
    ]], {
        1
    })

test:do_catchsql_test(
    "gh-4230-23",
    [[
        SELECT x FROM t2 WHERE '1' IN (x);
    ]], {
        1, "Type mismatch: can not convert integer to text"
    })

test:do_catchsql_test(
    "gh-4230-24",
    [[
        SELECT x FROM t2 WHERE '1.0' IN (x);
    ]], {
        1, "Type mismatch: can not convert integer to text"
    })

test:do_execsql_test(
    "gh-4230-25",
    [[
        SELECT x FROM t3 WHERE x IN (1);
    ]], {
        1
    })

test:do_execsql_test(
    "gh-4230-26",
    [[
        SELECT x FROM t3 WHERE x IN (1.0);
    ]], {
        1
    })

test:do_catchsql_test(
    "gh-4230-27",
    [[
        SELECT x FROM t3 WHERE x IN ('1');
    ]], {
        1, "Type mismatch: can not convert double to text"
    })

test:do_catchsql_test(
    "gh-4230-28",
    [[
        SELECT x FROM t3 WHERE x IN ('1.0');
    ]], {
        1, "Type mismatch: can not convert double to text"
    })

test:do_execsql_test(
    "gh-4230-29",
    [[
        SELECT x FROM t3 WHERE 1 IN (x);
    ]], {
        1
    })

test:do_execsql_test(
    "gh-4230-30",
    [[
        SELECT x FROM t3 WHERE 1.0 IN (x);
    ]], {
        1
    })

test:do_catchsql_test(
    "gh-4230-31",
    [[
        SELECT x FROM t3 WHERE '1' IN (x);
    ]], {
        1, "Type mismatch: can not convert double to text"
    })

test:do_catchsql_test(
    "gh-4230-32",
    [[
        SELECT x FROM t3 WHERE '1.0' IN (x);
    ]], {
        1, "Type mismatch: can not convert double to text"
    })

test:finish_test()
