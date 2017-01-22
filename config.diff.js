{
	"instances": [
		{
			"iot_id" : 1,
			"type" : EVSRC,
			"mid" : 3,
			"group_id" : 0,
			"mode_id" : 0,
			"host_id" : 1, //determined by first device
			"modtime" : 1234567890,
			"params" : {
				"devices" : [
					{
						"host_id" : 1,
						"device_ident" : "1000001.1.0"
					}
				],
				"config" : null
			}
		},
		{
			"iot_id" : 2,
			"type" : ACT,
			"mid" : 5,
			"group_id" : 0,
			"mode_id" : 0,
			"host_id" : 1, //determined by first input
			"modtime" : 1234567890,
			"params" : {
				"inputs" : [1],
				"config" : {
					"event-type" : 0,
					"key-code" : 0
				}
			}
		},
		{
			"iot_id" : 3,
			"type" : EXEC,
			"mid" : 4,
			"group_id" : 0,
			"mode_id" : 0,
			"host_id" : 1, //determined by first device
			"modtime" : 1234567890,
			"params" : {
				"devices" : [
					{
						"host_id" : 1, //iot_id of gateway
						"device_ident" : "1000001.1.0"
					}
				],
				"config" : null
			}
		},
		{
			"iot_id" : 4,
			"type" : ACTLIST,
			"group_id" : 0,
			"mode_id" : 0,
			"host_id" : 1, //determined by activator
			"modtime" : 1234567890,
			"params" : {
				"activator" : 2,
				"actions" : [
					{
						"executor" : 3,
						"action" : ENABLE,
						"params" : {
							"led" : 0
						}
					}
				]
			}
		}
	],
	"device_detections" : [
		{
			"iot_id" : 98,
			"host_id" : 1,
			"mid" : 9, //module id of manually controlled detector
			"modtime" : 1234567890
			"params" : [ //list of several detection params for same detection module id and host
				{
					"vendor" : 1234,
					"product" : 2345,
					"comport" : 1
				},
				{
					"host" : "1.2.3.4",
					"port" : 123
				}
			]
		}
	],
	"device_params" : [ //manually set params for driver of device. Any usecase?
		{
			"iot_id" : 99,
			"host_id" : 1, //iot_id of gateway
			"device_ident" : "1000001.1.0",
			"modtime" : 1234567890
			"params" : null
		}
	],
	"modes" : [ //per-group_id current mode of config. is part of general state
		{
			"group_id" : 0,
			"mode_id" : 0,
			"modtime" : 1234567890
		}
	],
	"hosts" : [ //taken from server-common table iot_gateways
		{
			"host_id" : 1, //some global ID of gateway
			"serial" : "S12345678",
			"publickey" : "kjhasdkjhasd",
			"local_ip" : "192.168.0.2", //reported by gateway during last connect to unet server
			"listen_port" : 12000, //reported by gateway port on local_ip to accept connections from other gateways. will be reused of possible in case of restart
			"actual_ip" : "193.1.2.3", //actual IP seen by unet server
			"connect_host" : "mygw.dyndns.org", //manually specified by user host/IP to allow connection to gateway
			"connect_port" : 12001, //manually specified by user to override listen_port to allow connection to gateway. Must connect to listen_port.
			"modtime" : 1234567890
		}
	]
	"deleted" : {
		"instances": [
			{
				"iot_id" : 100,
				"modtime" : 1234567890
			}
		],
		"device_detections" : [
			{
				"iot_id" : 98,
				"modtime" : 1234567890
			}
		],
		"device_params" : [
			{
				"iot_id" : 99,
				"modtime" : 1234567890
			}
		],
		"modes" : [ //per-group_id current mode of config. is part of general state
			{
				"group_id" : 0,
				"modtime" : 1234567890
			}
		],
		"hosts" : [
			{
				"host_id" : 1, //some global ID of gateway
				"modtime" : 1234567890
			}
		]
	}
}
