/*
 * Copyright (c) 2015-2016, Thomas Keh
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
 
#include "device.h"
#include "core.h"
#include "utils.h"
#include "logger.h"
#include "dictionary_error.h"
#include "profiles.h"
#include "sdo_error.h"
#include "global_config.h"

#include <cassert>
#include <algorithm>
#include <functional>
#include <memory>

namespace kaco {

Device::Device(Core& core, uint8_t node_id)
	: m_core(core), m_node_id(node_id), m_eds_library(m_dictionary, m_name_to_address) { }

Device::~Device() 
	{ }

bool Device::start() {

	m_core.nmt.send_nmt_message(m_node_id,NMT::Command::start_node);

	bool success = m_eds_library.lookup_library();

	if (!success) {
		ERROR("[Device::start] EDS library not found. Please fix or manage dictionary by yourself.");
	} else {
		success = m_eds_library.load_mandatory_entries();
		if (!success) {
			ERROR("[Device::start] Could not load mandatory dictionary entries. Please fix or manage dictionary by yourself.");
		}
	}

	success = load_operations() && success;
	success = load_constants() && success;
	return success;

}

uint8_t Device::get_node_id() const {
	return m_node_id;
}

bool Device::has_entry(const std::string& entry_name) {

	const std::string name = Utils::escape(entry_name);
	return m_name_to_address.count(name) != 0; // TODO DEBUG( && m_dictionary[m_name_to_address[name]].count() != 0 );

}

Value Device::get_entry_via_sdo(uint32_t index, uint8_t subindex, Type type) {

	sdo_error last_error(sdo_error::type::unknown);

	for (size_t i=0; i<Config::repeats_on_sdo_timeout+1; ++i) {
		try {
			std::vector<uint8_t> data = m_core.sdo.upload(m_node_id, index, subindex);
			return Value(type, data);
		} catch (const sdo_error& error) {
			last_error = error;
			if (i<Config::repeats_on_sdo_timeout) {
				DEBUG_LOG("[Device::get_entry_via_sdo] "<<error.what()<<" -> Repetition "<<std::to_string(i+1)
					<<" of "<<std::to_string(Config::repeats_on_sdo_timeout+1)<<".");
			}
		}
	}
	
	throw sdo_error(sdo_error::type::response_timeout, "Device::get_entry_via_sdo() failed after "
		+std::to_string(Config::repeats_on_sdo_timeout+1)+" repeats. Last error: "+std::string(last_error.what()));

}

const Value& Device::get_entry(const std::string& entry_name, uint8_t array_index, ReadAccessMethod access_method) {

	const std::string name = Utils::escape(entry_name);

	if (!has_entry(name)) {
		throw dictionary_error(dictionary_error::type::unknown_entry, name);
	}

	Entry& entry = m_dictionary[m_name_to_address[name]];

	if (array_index > 0 && !entry.is_array) {
		throw dictionary_error(dictionary_error::type::no_array, name);
	}

	if (access_method==ReadAccessMethod::sdo || (access_method==ReadAccessMethod::use_default && entry.read_access_method==ReadAccessMethod::sdo)) {
		
		DEBUG_LOG("[Device::get_entry] update_on_read.");

		uint8_t subindex = (entry.is_array) ? 0x1+array_index : entry.subindex;
		entry.set_value(get_entry_via_sdo(entry.index, subindex, entry.type), array_index);

	}

	return entry.get_value(array_index);

}

Type Device::get_entry_type(const std::string& entry_name) {

	const std::string name = Utils::escape(entry_name);

	if (!has_entry(name)) {
		throw dictionary_error(dictionary_error::type::unknown_entry, name);
	}

	return m_dictionary[m_name_to_address[name]].get_type();

}

void Device::set_entry_via_sdo(uint32_t index, uint8_t subindex, const Value& value) {

	sdo_error last_error(sdo_error::type::unknown);

	for (size_t i=0; i<Config::repeats_on_sdo_timeout+1; ++i) {
		try {
			const auto& bytes = value.get_bytes();
			m_core.sdo.download(m_node_id,index,subindex,bytes.size(),bytes);
			return;
		} catch (const sdo_error& error) {
			last_error = error;
			if (i<Config::repeats_on_sdo_timeout) {
				DEBUG_LOG("[Device::set_entry_via_sdo] "<<error.what()<<" -> Repetition "<<std::to_string(i+1)
					<<" of "<<std::to_string(Config::repeats_on_sdo_timeout+1)<<".");
			}
		}
	}

	throw sdo_error(sdo_error::type::response_timeout, "Device::set_entry_via_sdo() failed after "
		+std::to_string(Config::repeats_on_sdo_timeout+1)+" repeats. Last error: "+std::string(last_error.what()));

}

void Device::set_entry(const std::string& entry_name, const Value& value, uint8_t array_index, WriteAccessMethod access_method) {

	const std::string name = Utils::escape(entry_name);

	if (!has_entry(name)) {
		throw dictionary_error(dictionary_error::type::unknown_entry, name);
	}

	Entry& entry = m_dictionary[m_name_to_address[name]];

	if (array_index > 0 && !entry.is_array) {
		throw dictionary_error(dictionary_error::type::no_array, name);
	}

	if (value.type != entry.type) {
		throw dictionary_error(dictionary_error::type::wrong_type, name,
			"Entry type: "+Utils::type_to_string(entry.type)+", given type: "+Utils::type_to_string(value.type));
	}

	entry.set_value(value, array_index);

	if (access_method==WriteAccessMethod::sdo || (access_method==WriteAccessMethod::use_default && entry.write_access_method==WriteAccessMethod::sdo)) {

		DEBUG_LOG("[Device::set_entry] update_on_write.");

		const uint8_t subindex = (entry.is_array) ? 0x1+array_index : entry.subindex;
		set_entry_via_sdo(entry.index, subindex, value);

	}

}

void Device::add_receive_pdo_mapping(uint16_t cob_id, const std::string& entry_name, uint8_t offset, uint8_t array_index) {

	// TODO: update entry's default access method

	const std::string name = Utils::escape(entry_name);

	if (!has_entry(name)) {
		throw dictionary_error(dictionary_error::type::unknown_entry, name);
	}

	Entry& entry = m_dictionary[m_name_to_address[name]];

	if (array_index > 0 && !entry.is_array) {
		throw dictionary_error(dictionary_error::type::no_array, name);
	}

	const uint8_t type_size = Utils::get_type_size(entry.type);

	if (offset+type_size > 8) {
		throw dictionary_error(dictionary_error::type::mapping_size, name,
			"offset ("+std::to_string(offset)+") + type_size ("+std::to_string(type_size)+") > 8.");
	}
	

	ReceivePDOMapping *pdo_temp;

	{
		std::lock_guard<std::mutex> lock(m_receive_pdo_mappings_mutex);
		m_receive_pdo_mappings.push_front({cob_id,name,offset,array_index});
		pdo_temp = &m_receive_pdo_mappings.front();
	}
	
	ReceivePDOMapping& pdo = *pdo_temp;

	// TODO: this only works while add_pdo_received_callback takes callback by value.
	auto binding = std::bind(&Device::pdo_received_callback, this, pdo, std::placeholders::_1);
	m_core.pdo.add_pdo_received_callback(cob_id, std::move(binding));

}


void Device::add_transmit_pdo_mapping(uint16_t cob_id, const std::vector<Mapping>& mappings, TransmissionType transmission_type, std::chrono::milliseconds repeat_time) {

	TransmitPDOMapping *pdo_temp;

	{
		std::lock_guard<std::mutex> lock(m_transmit_pdo_mappings_mutex); // unlocks in case of exception
		// Contructor can throw dictionary_error. Letting user handle this.
		m_transmit_pdo_mappings.emplace_front(m_core, m_dictionary, m_name_to_address, cob_id, transmission_type, repeat_time, mappings);
		pdo_temp = &m_transmit_pdo_mappings.front();
	}
	
	TransmitPDOMapping& pdo = *pdo_temp;

	if (transmission_type==TransmissionType::ON_CHANGE) {

		for (const Mapping& mapping : pdo.mappings) {

			const std::string entry_name = Utils::escape(mapping.entry_name);

			// entry exists because check_correctness() == true.
			Entry& entry = m_dictionary.at(m_name_to_address.at(entry_name));

			entry.add_value_changed_callback([entry_name, &pdo](const Value& value){
				DEBUG_LOG("[Callback] Value of "<<entry_name<<" changed to "<<value);
				pdo.send();
			});
		}

	} else {

		// transmission_type==TransmissionType::PERIODIC

		if (repeat_time == std::chrono::milliseconds(0)) {
			WARN("[Device::add_transmit_pdo_mapping] Repeat time is 0. This could overload the bus.");
		}

		pdo.transmitter = std::unique_ptr<std::thread>(new std::thread([&pdo, repeat_time](){

			while (true) {
				DEBUG_LOG("[Timer thread] Sending periodic PDO.");
				pdo.send();
				std::this_thread::sleep_for(repeat_time);
			}

		}));

	}

}

void Device::pdo_received_callback(const ReceivePDOMapping& mapping, std::vector<uint8_t> data) {
	
	DEBUG_LOG("[Device::pdo_received_callback] Received a PDO for mapping '"<<mapping.entry_name<<"'!");

	const std::string entry_name = Utils::escape(mapping.entry_name);
	Entry& entry = m_dictionary[m_name_to_address[entry_name]];
	const uint8_t array_index = mapping.array_index;
	const uint8_t offset = mapping.offset;
	const uint8_t type_size = Utils::get_type_size(entry.type);

	if (data.size() < offset+type_size) {
		// We don't throw an exception here, because this could be a network error.
		ERROR("[Device::pdo_received_callback] PDO has wrong size!");
		DUMP(data.size());
		DUMP(offset);
		DUMP(type_size);
	}

	DEBUG_LOG("Updating entry "<<entry.name<<" (in case it's an array, index="<<array_index<<")");
	std::vector<uint8_t> bytes(data.begin()+offset, data.begin()+offset+type_size);
	entry.set_value(Value(entry.type,bytes), array_index);

}

uint16_t Device::get_device_profile_number() {
	uint32_t device_type = get_entry("Device type");
	return (device_type & 0xFFFF);
}

void Device::load_dictionary_from_library() {

	if (m_eds_library.ready()) {

		uint16_t profile = get_device_profile_number();

		DEBUG_LOG("Device::load_dictionary_from_library()...");

		// First, we try to load manufacturer specific entries.

		Config::eds_library_clear_dictionary = true;
		bool success = m_eds_library.load_manufacturer_eds(*this);
		Config::eds_library_clear_dictionary = false;
		
		if (success) {
			DEBUG_LOG("[Device::load_dictionary_from_library] Device "<<std::to_string(m_node_id)<<": Successfully loaded manufacturer-specific dictionary: " << m_eds_library.get_most_recent_eds_file_path());
			DEBUG_LOG("[Device::load_dictionary_from_library] Now we will add additional mappings from standard conformal entry names to the entries...");
			Config::eds_reader_just_add_mappings = true;
		} else {
			DEBUG_LOG("[Device::load_dictionary_from_library] Device "<<std::to_string(m_node_id)<<": There is no manufacturer-specific EDS file available. Going on with the default dictionary...");
			Config::eds_reader_just_add_mappings = false;
		}

		// Load entries like they are defined in the CiA CANopen standard documents...

		Config::eds_reader_mark_entries_as_generic = true;
		Config::eds_library_clear_dictionary = false;
		success = m_eds_library.load_default_eds(profile);

		if (success) {
			DEBUG_LOG("[Device::load_dictionary_from_library] Device "<<std::to_string(m_node_id)<<": Successfully loaded profile-specific dictionary: " << m_eds_library.get_most_recent_eds_file_path());
		} else {
			Config::eds_library_clear_dictionary = false;
			success = m_eds_library.load_mandatory_entries();
			if (success) {
				DEBUG_LOG("[Device::load_dictionary_from_library] Device "<<std::to_string(m_node_id)<<": Successfully loaded mandatory entries: " << m_eds_library.get_most_recent_eds_file_path());
			} else {
				throw canopen_error("Could not load mandatory CiA 301 dictionary entries for device with ID "+std::to_string(m_node_id)+". This can break various parts of KaCanOpen!");
			}
		}

		Config::eds_reader_mark_entries_as_generic = false;

	} else {
		throw canopen_error("[Device::load_dictionary_from_library] EDS library is not available.");
	}

}

bool Device::load_dictionary_from_eds(std::string path) {

	if (m_eds_library.ready()) {

		m_eds_library.reset_dictionary();
		Config::eds_reader_just_add_mappings = false;
		Config::eds_reader_mark_entries_as_generic = false;

		EDSReader reader(m_dictionary, m_name_to_address);
		bool success = reader.load_file(path);

		if (!success) {
			ERROR("[EDSLibrary::load_dictionary_from_eds] Loading file not successful.");
			return false;
		}

		success = reader.import_entries();

		if (!success) {
			ERROR("[EDSLibrary::load_dictionary_from_eds] Importing entries failed.");
			return false;
		}

	} else {
		ERROR("[Device::load_dictionary_from_eds] EDS library not ready. Please manage dictionary by yourself.");
		return false;
	}

	return true;

}

bool Device::load_operations() {
	const uint16_t profile = get_device_profile_number();
	if (Profiles::operations.count(profile)>0) {
		m_operations.insert(Profiles::operations.at(profile).cbegin(),Profiles::operations.at(profile).cend());
		return true;
	}
	return false;
}

void Device::add_operation(const std::string& operation_name, const Operation& operation) {
	const std::string name = Utils::escape(operation_name);
	if (m_operations.count(name)>0) {
		WARN("[Device::add_operation] Overwriting operation \""<<name<<"\".");
	}
	m_operations[name] = operation;
}

Value Device::execute(const std::string& operation_name, const Value& argument) {
	const std::string name = Utils::escape(operation_name);
	if (m_operations.count(name) == 0) {
		throw dictionary_error(dictionary_error::type::unknown_operation, name);
	}
	return m_operations[name](*this,argument);
}

bool Device::load_constants() {
	const uint16_t profile = get_device_profile_number();
	if (Profiles::constants.count(profile)>0) {
		m_constants.insert(Profiles::constants.at(profile).cbegin(),Profiles::constants.at(profile).cend());
		return true;
	}
	return false;
}

void Device::add_constant(const std::string& constant_name, const Value& constant) {
	const std::string name = Utils::escape(constant_name);
	if (m_constants.count(name)>0) {
		WARN("[Device::add_constant] Overwriting constant \""<<name<<"\".");
	}
	m_constants[name] = constant;
}

const Value& Device::get_constant(const std::string& constant_name) const {
	const std::string name = Utils::escape(constant_name);
	if (m_constants.count(name) == 0) {
		throw dictionary_error(dictionary_error::type::unknown_constant, name);
	}
	return m_constants.at(name);
}

void Device::print_dictionary() const {

	using EntryRef = std::reference_wrapper<const kaco::Entry>;
	std::vector<EntryRef> entries;

	for (const auto& pair : m_dictionary) {
		if (!pair.second.disabled) {
			entries.push_back(std::ref(pair.second));
		}
	}

	// sort by index and subindex
	std::sort(entries.begin(), entries.end(),
		[](const EntryRef& l, const EntryRef& r) { return l.get()<r.get(); });

	for (const auto& entry : entries) {
		entry.get().print();
	}

}

void Device::read_complete_dictionary() {
	for (auto& pair : m_dictionary) {
		try {
			get_entry(pair.second.name);
		} catch (const sdo_error& error) {
			pair.second.disabled = true;
			DEBUG_LOG("[Device::read_complete_dictionary] SDO error for field "<<pair.second.name<<": "<<error.what()<<" -> disable entry.");
		}
	}
}


const Value Device::m_dummy_value = Value();

} // end namespace kaco