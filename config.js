{
	"my_host_id" : 1,
	"config_modtime" : 1234567890, //modtime of latest applied config update
	"config" : CONFIG_STRUCTURE from config.diff.js without "deleted" clouse
	"persistent_states" : [
		{
			"iot_id" : 1,
			"type" : EVSRC,
			"mid" : 3,
			"state" : STATEDATA as assigned by corresponding instance during deinit
		}
	]
}
