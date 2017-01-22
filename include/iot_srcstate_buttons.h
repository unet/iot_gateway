#ifndef IOT_SRCSTATE_BUTTONS_H
#define IOT_SRCSTATE_BUTTONS_H
//represents state data for both IOT_SRCSTATE_CLASSID_BUTTONS and IOT_SRCSTATE_CLASSID_KEYBOARD

#include <stdint.h>

#include <iot_srcstate.h>
#include <iot_utils.h>

#include <linux/input-event-codes.h> //TODO use some compat for windows


//Represents state of buttons. Can be used for real keyboards and any abstraction of button which can have depressed and released state
//Each button is identified by macro code from /usr/include/linux/input-event-codes.h like 'KEY_0', 'KEY_A'
struct iot_srcstate_data_buttons_t {
	uint32_t statusmap[(KEY_CNT+31)/32]; //bitmap of buttons pressed status

//Methods
	//Test if specific key is down
	uint32_t is_down(uint16_t key_id) {
		if(key_id>=KEY_CNT) return false;
		return bitmap32_test_bit(statusmap, key_id);
	}
};

struct iot_srcstate_data_keyboard_t : public iot_srcstate_data_buttons_t {
	bool is_ctrl_down(void) { //checks if any CTRL button is down
		return is_down(KEY_LEFTCTRL) || is_down(KEY_RIGHTCTRL);
	}
	bool is_shift_down(void) { //checks if any SHIFT button is down
		return is_down(KEY_LEFTSHIFT) || is_down(KEY_RIGHTSHIFT);
	}
	bool is_alt_down(void) { //checks if any ALT button is down
		return is_down(KEY_LEFTALT) || is_down(KEY_RIGHTALT);
	}
	bool is_meta_down(void) { //checks if any META button is down
		return is_down(KEY_LEFTMETA) || is_down(KEY_RIGHTMETA);
	}
};

//Mandatory static methods
	//calculates required storage in bytes by start address of data or returns negative error code (non-parsable data provided)
//	static ssize_t get_size(void *buf);
	//extracts data from buf into provided struct. dstsize must be at least size returned by get_size(). Returns actual size of data
//	static ssize_t extract(void *buf, iot_srcstate_data_buttons_t* dst, ssize_t dstsize);


#endif //IOT_SRCSTATE_BUTTONS_H