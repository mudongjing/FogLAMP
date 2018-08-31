#ifndef _NORTH_PLUGIN
#define _NORTH_PLUGIN
/*
 * FogLAMP south service.
 *
 * Copyright (c) 2018 Dianomic Systems
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */

#include <plugin.h>
#include <plugin_manager.h>
#include <reading.h>
#include <config_category.h>

/**
 * Class that represents a north plugin.
 *
 * The purpose of this class is to hide the use of the pointers into the
 * dynamically loaded plugin and wrap the interface into a class that
 * can be used directly in the north subsystem.
 *
 * This is achieved by having a set of private member variables which are
 * the pointers to the functions in the plugin, and a set of public methods
 * that will call these functions via the function pointers.
 */
class NorthPlugin : public Plugin {
	public:
		// Methods
		NorthPlugin(const PLUGIN_HANDLE handle);
		~NorthPlugin();

		void			shutdown();
		PLUGIN_INFORMATION* 	info() const;
		std::string& 		extra_config() const;
		uint32_t		send(const std::vector<Reading* >& readings) const;
		PLUGIN_HANDLE		init(const ConfigCategory& config);

	private:
		// Function pointers
		void			(*pluginShutdownPtr)(const PLUGIN_HANDLE);
		PLUGIN_INFORMATION*	(*pluginInfo)();
		std::string&		(*pluginExtraConfig)();
		uint32_t		(*pluginSend)(const PLUGIN_HANDLE,
						      const std::vector<Reading* >& readings);
		PLUGIN_HANDLE		(*pluginInit)(const ConfigCategory* config);

	private:
		// Attributes
		PLUGIN_HANDLE		m_instance;
};

#endif
