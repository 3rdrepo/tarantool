#!/usr/bin/env tarantool
--
-- gh-4668: box.info:memory() displayed full content of box.info
--
local tap = require('tap')
local test = tap.test("Tarantool 4668")
test:plan(1)

box.cfg()

a = box.info.memory()
b = box.info:memory()

test:is(table.concat(a), table.concat(b), "box.info:memory")
os.exit(0)
