idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
		"common/display.c"
		"common/theme.c"
		"icons.c"
		"message_dialog.c"
		"textedit.c"
		"util_ssh.c"
		"settings_ssh.c"
		"menu_ssh.c"
		"menu_ssh_edit.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		lwip
		robotman2412__pax-gfx
		robotman2412__pax-codecs
		nicolaielectronics__wifi-manager
		skuodi__libssh2_esp
		badgeteam__terminal-emulator
		gui
		${extra_requires}
	INCLUDE_DIRS
		"."
	EMBED_FILES
		"icons/esc.png"
		"icons/f1.png"
		"icons/f2.png"
		"icons/f3.png"
		"icons/f4.png"
		"icons/f5.png"
		"icons/f6.png"
		"icons/error.png"
		"icons/help.png"
		"icons/extension.png"
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
