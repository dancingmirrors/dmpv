/* Copyright (C) 2017 the mpv developers
 * SPDX-License-Identifier: ISC
 *
 * This header provides compatibility layer for applications expecting
 * standard libmpv API while using libdmpv implementation.
 */

#ifndef MPV_CLIENT_API_H_
#define MPV_CLIENT_API_H_

/* Include the actual dmpv client API
 * Note: This relative path assumes the standard dmpv source tree structure
 * where include/mpv/ is at the root level alongside misc/
 */
#include "../../misc/client.h"

/* Map mpv_* types and functions to dmpv_* equivalents */
#define mpv_handle dmpv_handle
#define mpv_error dmpv_error
#define mpv_format dmpv_format
#define mpv_node dmpv_node
#define mpv_node_list dmpv_node_list
#define mpv_byte_array dmpv_byte_array
#define mpv_event_id dmpv_event_id
#define mpv_event dmpv_event
#define mpv_event_property dmpv_event_property
#define mpv_event_log_message dmpv_event_log_message
#define mpv_event_start_file dmpv_event_start_file
#define mpv_event_end_file dmpv_event_end_file
#define mpv_event_client_message dmpv_event_client_message
#define mpv_event_hook dmpv_event_hook
#define mpv_event_command dmpv_event_command
#define mpv_log_level dmpv_log_level
#define mpv_end_file_reason dmpv_end_file_reason

/* Map error codes */
#define MPV_ERROR_SUCCESS DMPV_ERROR_SUCCESS
#define MPV_ERROR_EVENT_QUEUE_FULL DMPV_ERROR_EVENT_QUEUE_FULL
#define MPV_ERROR_NOMEM DMPV_ERROR_NOMEM
#define MPV_ERROR_UNINITIALIZED DMPV_ERROR_UNINITIALIZED
#define MPV_ERROR_INVALID_PARAMETER DMPV_ERROR_INVALID_PARAMETER
#define MPV_ERROR_OPTION_NOT_FOUND DMPV_ERROR_OPTION_NOT_FOUND
#define MPV_ERROR_OPTION_FORMAT DMPV_ERROR_OPTION_FORMAT
#define MPV_ERROR_OPTION_ERROR DMPV_ERROR_OPTION_ERROR
#define MPV_ERROR_PROPERTY_NOT_FOUND DMPV_ERROR_PROPERTY_NOT_FOUND
#define MPV_ERROR_PROPERTY_FORMAT DMPV_ERROR_PROPERTY_FORMAT
#define MPV_ERROR_PROPERTY_UNAVAILABLE DMPV_ERROR_PROPERTY_UNAVAILABLE
#define MPV_ERROR_PROPERTY_ERROR DMPV_ERROR_PROPERTY_ERROR
#define MPV_ERROR_COMMAND DMPV_ERROR_COMMAND
#define MPV_ERROR_LOADING_FAILED DMPV_ERROR_LOADING_FAILED
#define MPV_ERROR_AO_INIT_FAILED DMPV_ERROR_AO_INIT_FAILED
#define MPV_ERROR_VO_INIT_FAILED DMPV_ERROR_VO_INIT_FAILED
#define MPV_ERROR_NOTHING_TO_PLAY DMPV_ERROR_NOTHING_TO_PLAY
#define MPV_ERROR_UNKNOWN_FORMAT DMPV_ERROR_UNKNOWN_FORMAT
#define MPV_ERROR_UNSUPPORTED DMPV_ERROR_UNSUPPORTED
#define MPV_ERROR_NOT_IMPLEMENTED DMPV_ERROR_NOT_IMPLEMENTED
#define MPV_ERROR_GENERIC DMPV_ERROR_GENERIC

/* Map format types */
#define MPV_FORMAT_NONE DMPV_FORMAT_NONE
#define MPV_FORMAT_STRING DMPV_FORMAT_STRING
#define MPV_FORMAT_OSD_STRING DMPV_FORMAT_OSD_STRING
#define MPV_FORMAT_FLAG DMPV_FORMAT_FLAG
#define MPV_FORMAT_INT64 DMPV_FORMAT_INT64
#define MPV_FORMAT_DOUBLE DMPV_FORMAT_DOUBLE
#define MPV_FORMAT_NODE DMPV_FORMAT_NODE
#define MPV_FORMAT_NODE_ARRAY DMPV_FORMAT_NODE_ARRAY
#define MPV_FORMAT_NODE_MAP DMPV_FORMAT_NODE_MAP
#define MPV_FORMAT_BYTE_ARRAY DMPV_FORMAT_BYTE_ARRAY

/* Map event IDs */
#define MPV_EVENT_NONE DMPV_EVENT_NONE
#define MPV_EVENT_SHUTDOWN DMPV_EVENT_SHUTDOWN
#define MPV_EVENT_LOG_MESSAGE DMPV_EVENT_LOG_MESSAGE
#define MPV_EVENT_GET_PROPERTY_REPLY DMPV_EVENT_GET_PROPERTY_REPLY
#define MPV_EVENT_SET_PROPERTY_REPLY DMPV_EVENT_SET_PROPERTY_REPLY
#define MPV_EVENT_COMMAND_REPLY DMPV_EVENT_COMMAND_REPLY
#define MPV_EVENT_START_FILE DMPV_EVENT_START_FILE
#define MPV_EVENT_END_FILE DMPV_EVENT_END_FILE
#define MPV_EVENT_FILE_LOADED DMPV_EVENT_FILE_LOADED
#define MPV_EVENT_IDLE DMPV_EVENT_IDLE
#define MPV_EVENT_TICK DMPV_EVENT_TICK
#define MPV_EVENT_CLIENT_MESSAGE DMPV_EVENT_CLIENT_MESSAGE
#define MPV_EVENT_VIDEO_RECONFIG DMPV_EVENT_VIDEO_RECONFIG
#define MPV_EVENT_AUDIO_RECONFIG DMPV_EVENT_AUDIO_RECONFIG
#define MPV_EVENT_SEEK DMPV_EVENT_SEEK
#define MPV_EVENT_PLAYBACK_RESTART DMPV_EVENT_PLAYBACK_RESTART
#define MPV_EVENT_PROPERTY_CHANGE DMPV_EVENT_PROPERTY_CHANGE
#define MPV_EVENT_QUEUE_OVERFLOW DMPV_EVENT_QUEUE_OVERFLOW
#define MPV_EVENT_HOOK DMPV_EVENT_HOOK

/* Map log levels */
#define MPV_LOG_LEVEL_NONE DMPV_LOG_LEVEL_NONE
#define MPV_LOG_LEVEL_FATAL DMPV_LOG_LEVEL_FATAL
#define MPV_LOG_LEVEL_ERROR DMPV_LOG_LEVEL_ERROR
#define MPV_LOG_LEVEL_WARN DMPV_LOG_LEVEL_WARN
#define MPV_LOG_LEVEL_INFO DMPV_LOG_LEVEL_INFO
#define MPV_LOG_LEVEL_V DMPV_LOG_LEVEL_V
#define MPV_LOG_LEVEL_DEBUG DMPV_LOG_LEVEL_DEBUG
#define MPV_LOG_LEVEL_TRACE DMPV_LOG_LEVEL_TRACE

/* Map end file reasons */
#define MPV_END_FILE_REASON_EOF DMPV_END_FILE_REASON_EOF
#define MPV_END_FILE_REASON_STOP DMPV_END_FILE_REASON_STOP
#define MPV_END_FILE_REASON_QUIT DMPV_END_FILE_REASON_QUIT
#define MPV_END_FILE_REASON_ERROR DMPV_END_FILE_REASON_ERROR
#define MPV_END_FILE_REASON_REDIRECT DMPV_END_FILE_REASON_REDIRECT

/* Map API version macros */
#define MPV_MAKE_VERSION DMPV_MAKE_VERSION
#define MPV_CLIENT_API_VERSION DMPV_CLIENT_API_VERSION
#define MPV_ENABLE_DEPRECATED DMPV_ENABLE_DEPRECATED

/* Map function names */
#define mpv_client_api_version dmpv_client_api_version
#define mpv_error_string dmpv_error_string
#define mpv_free dmpv_free
#define mpv_client_name dmpv_client_name
#define mpv_client_id dmpv_client_id
#define mpv_create dmpv_create
#define mpv_initialize dmpv_initialize
#define mpv_destroy dmpv_destroy
#define mpv_terminate_destroy dmpv_terminate_destroy
#define mpv_create_client dmpv_create_client
#define mpv_create_weak_client dmpv_create_weak_client
#define mpv_load_config_file dmpv_load_config_file
#define mpv_get_time_us dmpv_get_time_us
#define mpv_free_node_contents dmpv_free_node_contents
#define mpv_set_option dmpv_set_option
#define mpv_set_option_string dmpv_set_option_string
#define mpv_command dmpv_command
#define mpv_command_node dmpv_command_node
#define mpv_command_ret dmpv_command_ret
#define mpv_command_string dmpv_command_string
#define mpv_command_async dmpv_command_async
#define mpv_command_node_async dmpv_command_node_async
#define mpv_abort_async_command dmpv_abort_async_command
#define mpv_set_property dmpv_set_property
#define mpv_set_property_string dmpv_set_property_string
#define mpv_del_property dmpv_del_property
#define mpv_set_property_async dmpv_set_property_async
#define mpv_get_property dmpv_get_property
#define mpv_get_property_string dmpv_get_property_string
#define mpv_get_property_osd_string dmpv_get_property_osd_string
#define mpv_get_property_async dmpv_get_property_async
#define mpv_observe_property dmpv_observe_property
#define mpv_unobserve_property dmpv_unobserve_property
#define mpv_event_name dmpv_event_name
#define mpv_event_to_node dmpv_event_to_node
#define mpv_request_event dmpv_request_event
#define mpv_request_log_messages dmpv_request_log_messages
#define mpv_wait_event dmpv_wait_event
#define mpv_wakeup dmpv_wakeup
#define mpv_set_wakeup_callback dmpv_set_wakeup_callback
#define mpv_wait_async_requests dmpv_wait_async_requests
#define mpv_hook_add dmpv_hook_add
#define mpv_hook_continue dmpv_hook_continue
#define mpv_get_wakeup_pipe dmpv_get_wakeup_pipe

#endif /* MPV_CLIENT_API_H_ */
