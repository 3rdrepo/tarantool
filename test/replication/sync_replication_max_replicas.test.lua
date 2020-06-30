env = require('test_run')
test_run = env.new()
engine = test_run:get_cfg('engine')

--box.schema.user.grant('guest', 'replication')
--box.cfg{replication_synchro_quorum=2, replication_synchro_timeout=0.1}

-- Max allowed instances in a cluster
MAX_INSTANCES = 32
--test_run:cmd("setopt delimiter ';'")
--for i=1,MAX_INSTANCES do
--    test_run:cmd(string.format('create server replica%s with rpl_master=default, \
--                                         script="replication/replica.lua"', i))
--    test_run:cmd(string.format('start server replica%s with wait=True, wait_load=True', i))
--end;
--
--test_run:cmd('create server replica1 with rpl_master=default, script="replication/replica.lua"')
--test_run:cmd('start server replica1 with wait=True, wait_load=True')

--test_run:cmd('create server replica2 with rpl_master=default, script="replication/replica.lua"')
--test_run:cmd('start server replica2 with wait=True, wait_load=True')

--test_run:cmd("setopt delimiter ''");
--test_run:cmd('switch default')
--_ = box.schema.space.create('sync', {is_sync=true, engine=engine})
--_ = box.space.sync:create_index('pk')

-- Successful write with replication_synchro_quorum=MAX_INSTANCES.
--box.cfg{replication_synchro_quorum=2}
--box.space.test:insert{1} -- success
--for i = 1,MAX_INSTANCES do
--    test_run:cmd('switch quorum'..i)
--    box.space.test:select{1} -- success
--end

-- Cleanup.
--test_run:cmd('switch default')
--test_run:cmd("setopt delimiter ';'")
--for i=1,MAX_INSTANCES do
--    test_run:cmd(string.format('stop server replica%d', i))
--    test_run:cmd(string.format('delete server replica%d', i))
--end;
--test_run:cmd("setopt delimiter ''")
--box.space.sync:drop()
--box.schema.user.revoke('guest', 'replication')



-- Setup a cluster with 32 instances (TODO).
--SERVERS = {'quorum1', 'quorum2', 'quorum3'}
--test_run:create_cluster(SERVERS, "replication", {args="0.1"})
--test_run:wait_fullmesh(SERVERS)
--test_run:cmd("switch quorum1")
--box.space.test:select()

-- Teardown.
--test_run:drop_cluster(SERVERS)
--
--test_run:cleanup_cluster()
