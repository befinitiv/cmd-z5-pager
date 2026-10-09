#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "factory.h"

LOG_MODULE_REGISTER(factory, CONFIG_CMDZ5_APP_LOG_LEVEL);

void factory_reset(void)
{
	const struct flash_area *fa;
	int ret;

	LOG_WRN("factory reset");

	/* Messages (the file system itself stays). */
	fs_unlink("/lfs/msgs.log");
	fs_unlink("/lfs/msgs.new");

	/*
	 * All settings: erase the whole storage partition. The settings
	 * subsystem starts with an empty partition after the restart.
	 */
	ret = flash_area_open(PARTITION_ID(storage_partition), &fa);
	if (ret == 0) {
		ret = flash_area_erase(fa, 0, fa->fa_size);
		flash_area_close(fa);
	}
	if (ret) {
		LOG_ERR("erasing the settings failed (%d)", ret);
	}
	log_flush();
	sys_reboot(SYS_REBOOT_COLD);
}
