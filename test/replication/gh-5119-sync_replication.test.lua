env = require('test_run')
test_run = env.new()
engine = test_run:get_cfg('engine')
fiber = require('fiber')

box.schema.user.grant('guest', 'replication')
box.cfg{replication_synchro_quorum=2, replication_synchro_timeout=0.1}

test_run:cmd('create server replica with rpl_master=default,\
                                         script="replication/replica.lua"')
test_run:cmd('start server replica with wait=True, wait_load=True')

_ = box.schema.space.create('sync', {is_sync=true, engine=engine})
_ = box.space.sync:create_index('pk')

test_run:switch('default')
box.cfg{replication_synchro_quorum=3, replication_synchro_timeout=30}
test_run:cmd("setopt delimiter ';'")
_ = fiber.create(function()
    box.space.sync:insert{1}
end);
test_run:cmd("setopt delimiter ''");
box.cfg{replication_synchro_quorum=2, replication_synchro_timeout=0.1}
box.space.sync:insert{1}
box.space.sync:insert{2}
box.space.sync:insert{3}
test_run:switch('replica')
box.space.sync:select{} -- 1, 2, 3
test_run:switch('default')
box.space.sync:truncate()

-- Cleanup.
test_run:cmd('switch default')
test_run:cmd('stop server replica')
test_run:cmd('delete server replica')
box.space.sync:drop()
box.schema.user.revoke('guest', 'replication')
