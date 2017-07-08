{
	.module_id = 1,
	.bundle = &IOT_MODULESDB_BUNDLE_OBJ(unet, generic__inputlinux),
	.module_name = "detector",
	.autoload = true,
	.autostart_detector = true,
	.item = NULL
},
{
	.module_id = 2,
	.bundle = &IOT_MODULESDB_BUNDLE_OBJ(unet, generic__inputlinux),
	.module_name = "input_drv",
	.autoload = true,
	.autostart_detector = false,
	.item = NULL
},
{
	.module_id = 3,
	.bundle = &IOT_MODULESDB_BUNDLE_OBJ(unet, generic__kbd),
	.module_name = "kbd_src",
	.autoload = false,
	.autostart_detector = false,
	.item = NULL
},
{
	.module_id = 4,
	.bundle = &IOT_MODULESDB_BUNDLE_OBJ(unet, generic__kbd),
	.module_name = "oper_keys",
	.autoload = false,
	.autostart_detector = false,
	.item = NULL
},
{
	.module_id = 5,
	.bundle = &IOT_MODULESDB_BUNDLE_OBJ(unet, generic__kbd),
	.module_name = "exec_actable",
	.autoload = false,
	.autostart_detector = false,
	.item = NULL
},
{
	.module_id = 6,
	.bundle = &IOT_MODULESDB_BUNDLE_OBJ(unet, generic__kbd),
	.module_name = "exec_toneplayer",
	.autoload = false,
	.autostart_detector = false,
	.item = NULL
},
