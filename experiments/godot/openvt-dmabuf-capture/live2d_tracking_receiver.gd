extends RefCounted

const PROTOCOL_NAME := "vtb-live2d-parameters"
const PROTOCOL_VERSION := 1
const VIEW_ZOOM_MIN := 0.5
const VIEW_ZOOM_MAX := 6.0
const VIEW_OFFSET_MIN := -2.0
const VIEW_OFFSET_MAX := 2.0
const ALLOWED_PARAMETERS := {
	"ParamAngleX": true,
	"ParamAngleY": true,
	"ParamAngleZ": true,
	"ParamBodyAngleX": true,
	"ParamBodyAngleY": true,
	"ParamBodyAngleZ": true,
	"ParamEyeLOpen": true,
	"ParamEyeROpen": true,
	"ParamMouthOpenY": true,
	"ParamMouthForm": true,
}

var peer := PacketPeerUDP.new()
var last_sent_ns := -1
var source_clock_offset_usec := 0
var source_clock_offset_initialized := false
var profile_enabled := false
var profile_received_total := 0
var profile_valid_total := 0
var profile_accepted_total := 0
var profile_invalid_total := 0
var profile_out_of_order_total := 0
var profile_overwritten_total := 0
var profile_receive_usec_total := 0
var profile_parse_usec_total := 0
var profile_received_window := 0
var profile_valid_window := 0
var profile_accepted_window := 0
var profile_invalid_window := 0
var profile_out_of_order_window := 0
var profile_overwritten_window := 0
var profile_receive_usec_window := 0
var profile_parse_usec_window := 0


func set_profile_enabled(enabled: bool) -> void:
	profile_enabled = enabled


func open(bind_address: String, port: int) -> Error:
	if port < 1 or port > 65535:
		return ERR_INVALID_PARAMETER
	return peer.bind(port, bind_address)


func close() -> void:
	peer.close()


func receive_latest() -> Dictionary:
	var latest := {}
	while peer.get_available_packet_count() > 0:
		var receive_started_usec := 0
		if profile_enabled:
			receive_started_usec = Time.get_ticks_usec()
		var payload := peer.get_packet().get_string_from_utf8()
		var receive_finished_usec := 0
		var parse_started_usec := 0
		if profile_enabled:
			receive_finished_usec = Time.get_ticks_usec()
			parse_started_usec = receive_finished_usec
		var value = JSON.parse_string(payload)
		if profile_enabled:
			var parse_finished_usec := Time.get_ticks_usec()
			profile_received_total += 1
			profile_received_window += 1
			profile_receive_usec_total += receive_finished_usec - receive_started_usec
			profile_receive_usec_window += receive_finished_usec - receive_started_usec
			profile_parse_usec_total += parse_finished_usec - parse_started_usec
			profile_parse_usec_window += parse_finished_usec - parse_started_usec
		if typeof(value) != TYPE_DICTIONARY:
			_record_invalid_packet()
			continue
		var packet := value as Dictionary
		if not _packet_is_valid(packet):
			_record_invalid_packet()
			continue
		if profile_enabled:
			profile_valid_total += 1
			profile_valid_window += 1

		var sent_ns := int(packet["sent_ns"])
		if sent_ns <= last_sent_ns:
			if profile_enabled:
				profile_out_of_order_total += 1
				profile_out_of_order_window += 1
			continue
		last_sent_ns = sent_ns
		if profile_enabled:
			profile_accepted_total += 1
			profile_accepted_window += 1
			if not latest.is_empty():
				profile_overwritten_total += 1
				profile_overwritten_window += 1
		var received_at_usec := Time.get_ticks_usec()
		var sent_usec := int(sent_ns / 1000)
		var observed_offset_usec := received_at_usec - sent_usec
		if not source_clock_offset_initialized or observed_offset_usec < source_clock_offset_usec:
			source_clock_offset_usec = observed_offset_usec
			source_clock_offset_initialized = true
		packet["received_at_usec"] = received_at_usec
		packet["source_clock_offset_usec"] = source_clock_offset_usec
		latest = packet
	return latest


func take_profile_window() -> Dictionary:
	var snapshot := {
		"received": profile_received_window,
		"valid": profile_valid_window,
		"accepted": profile_accepted_window,
		"invalid": profile_invalid_window,
		"out_of_order": profile_out_of_order_window,
		"overwritten": profile_overwritten_window,
		"receive_usec": profile_receive_usec_window,
		"parse_usec": profile_parse_usec_window,
	}
	profile_received_window = 0
	profile_valid_window = 0
	profile_accepted_window = 0
	profile_invalid_window = 0
	profile_out_of_order_window = 0
	profile_overwritten_window = 0
	profile_receive_usec_window = 0
	profile_parse_usec_window = 0
	return snapshot


func profile_totals() -> Dictionary:
	return {
		"received": profile_received_total,
		"valid": profile_valid_total,
		"accepted": profile_accepted_total,
		"invalid": profile_invalid_total,
		"out_of_order": profile_out_of_order_total,
		"overwritten": profile_overwritten_total,
		"receive_usec": profile_receive_usec_total,
		"parse_usec": profile_parse_usec_total,
	}


func _record_invalid_packet() -> void:
	if not profile_enabled:
		return
	profile_invalid_total += 1
	profile_invalid_window += 1


func _packet_is_valid(packet: Dictionary) -> bool:
	if packet.get("protocol", "") != PROTOCOL_NAME:
		return false
	if int(packet.get("version", 0)) != PROTOCOL_VERSION:
		return false
	if not _numeric_field_is_valid(packet, "sent_ns"):
		return false
	if not _numeric_field_is_valid(packet, "points_ready_ns"):
		return false
	if int(packet["sent_ns"]) < int(packet["points_ready_ns"]):
		return false
	if not packet.has("parameters"):
		return false
	if typeof(packet["parameters"]) != TYPE_DICTIONARY:
		return false

	var parameters := packet["parameters"] as Dictionary
	for parameter in parameters:
		if not ALLOWED_PARAMETERS.has(parameter):
			return false
		var value = parameters[parameter]
		if typeof(value) != TYPE_FLOAT and typeof(value) != TYPE_INT:
			return false
		var numeric_value := float(value)
		if is_nan(numeric_value) or is_inf(numeric_value):
			return false

	if packet.has("view"):
		if typeof(packet["view"]) != TYPE_DICTIONARY:
			return false
		var view := packet["view"] as Dictionary
		for field in ["zoom", "offset_x", "offset_y"]:
			if not _numeric_field_is_valid(view, field):
				return false
		var zoom := float(view["zoom"])
		var offset_x := float(view["offset_x"])
		var offset_y := float(view["offset_y"])
		if zoom < VIEW_ZOOM_MIN or zoom > VIEW_ZOOM_MAX:
			return false
		if offset_x < VIEW_OFFSET_MIN or offset_x > VIEW_OFFSET_MAX:
			return false
		if offset_y < VIEW_OFFSET_MIN or offset_y > VIEW_OFFSET_MAX:
			return false
	return true


func _numeric_field_is_valid(packet: Dictionary, field: String) -> bool:
	if not packet.has(field):
		return false
	var value = packet[field]
	if typeof(value) != TYPE_FLOAT and typeof(value) != TYPE_INT:
		return false
	var numeric_value := float(value)
	return not is_nan(numeric_value) and not is_inf(numeric_value)
