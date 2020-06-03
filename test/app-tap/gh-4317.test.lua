#!/usr/bin/env tarantool

local tap = require('tap')
local console = require('console')
local fio = require('fio')

-- The following cases use LD_PRELOAD, so will not work under
-- Mac OS X.
if jit.os == 'OSX' then
    os.exit(0)
end

test = tap.test("console")
test:plan(1)

-- Allow to run the test from the repository root without
-- test-run:
--
-- $ ./src/tarantool test/app-tap/gh-4317.test.lua
--
-- It works at least for in-source build.
local is_under_test_run = pcall(require, 'test_run')
local isatty_lib_dir = is_under_test_run and '../..' or 'test/app-tap'
local isatty_lib_path = fio.pathjoin(isatty_lib_dir, 'libisatty.so')
local saved_path = os.getenv('PATH')
if not is_under_test_run then
    os.setenv('PATH', './src:', saved_path)
end

local tarantool_command = "local a = 0 \\\nfor i = 1, 10 do\na = a + i\nend \\\nprint(a)"
local result_str =  [[tarantool> local a = 0 \
         > for i = 1, 10 do
         > a = a + i
         > end \
         > print(a)
55
---
...

tarantool> ]]
local cmd = ([[printf '%s\n' | LD_PRELOAD='%s' tarantool ]] ..
[[2>/dev/null]]):format(tarantool_command, isatty_lib_path)
local fh = io.popen(cmd, 'r')
-- Readline on CentOS 7 produces \e[?1034h escape
-- sequence before tarantool> prompt, remove it.
local result = fh:read('*a'):gsub('\x1b%[%?1034h', '')
fh:close()
test:is(result, result_str, "Backslash")

test:check()
os.exit(0)
