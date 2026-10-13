#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include "command.hpp"

#include "localized_strings.hpp"
#include "console.hpp"
#include "game_module.hpp"
#include "fps.hpp"

#include "game/ui_scripting/execution.hpp"

#include "ui_scripting.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>
#include <utils/io.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string_view>

namespace ui_scripting
{
	namespace
	{
		std::unordered_map<game::hks::cclosure*, std::function<arguments(const function_arguments& args)>> converted_functions;

		utils::hook::detour hks_start_hook;
		utils::hook::detour hks_shutdown_hook;
		utils::hook::detour hks_package_require_hook;
		utils::hook::detour hks_lua_error_hook;
		utils::hook::detour hks_vm_call_internal_hook;

		struct script
		{
			std::string name;
			std::string root;
		};

		struct globals
		{
			std::string in_require_script;
			std::vector<script> loaded_scripts;
			bool load_raw_script{};
			std::string raw_script_name{};
		};

		globals globals{};
		bool running{};

		bool is_loaded_script(const std::string& name)
		{
			return std::ranges::any_of(globals.loaded_scripts, [name](const auto& loaded_script)
			{
				return loaded_script.name == name;
			});
		}

		std::string get_root_script(const std::string& name)
		{
			for (const auto& loaded_script : globals.loaded_scripts)
			{
				if (loaded_script.name == name)
				{
					return loaded_script.root;
				}
			}

			return {};
		}

		table get_globals()
		{
			const auto state = *game::hks::lua_state;
			return state->globals.v.table;
		}

		const char* hks_type_name(const game::hks::HksObjectType type)
		{
			switch (type)
			{
			case game::hks::TANY: return "any";
			case game::hks::TNONE: return "none";
			case game::hks::TNIL: return "nil";
			case game::hks::TBOOLEAN: return "boolean";
			case game::hks::TLIGHTUSERDATA: return "lightuserdata";
			case game::hks::TNUMBER: return "number";
			case game::hks::TSTRING: return "string";
			case game::hks::TTABLE: return "table";
			case game::hks::TFUNCTION: return "function";
			case game::hks::TUSERDATA: return "userdata";
			case game::hks::TTHREAD: return "thread";
			case game::hks::TIFUNCTION: return "lua-function";
			case game::hks::TCFUNCTION: return "c-function";
			case game::hks::TUI64: return "uint64";
			case game::hks::TSTRUCT: return "struct";
			default: return "unknown";
			}
		}

		void log_engine_status(const char* stage, const table& lua)
		{
			if (!utils::flags::has_flag("hks_lui_load_trace"))
			{
				return;
			}

			const auto engine_value = lua["Engine"];
			const auto engine_object_type = engine_value.get_raw().t;
			if (!engine_value.is<table>())
			{
				console::info(
					"[ui_scripting] HKS LUI trace: %s Engine=%s(%u), "
					"Engine.GetPlayercardEmblemTableName=<unavailable>, "
					"Engine.IsSteam=<unavailable>, Engine.IsGDK=<unavailable>, "
					"Engine.IsUbiConnect=<unavailable>\n",
					stage, hks_type_name(engine_object_type),
					static_cast<unsigned int>(engine_object_type));
				return;
			}

			const auto engine_type = engine_value.as<table>();
			const auto function_value = engine_type["GetPlayercardEmblemTableName"];
			const auto function_object_type = function_value.get_raw().t;
			const auto is_steam_value = engine_type["IsSteam"];
			const auto is_steam_object_type = is_steam_value.get_raw().t;
			const auto is_gdk_value = engine_type["IsGDK"];
			const auto is_gdk_object_type = is_gdk_value.get_raw().t;
			const auto is_ubi_connect_value = engine_type["IsUbiConnect"];
			const auto is_ubi_connect_object_type = is_ubi_connect_value.get_raw().t;
			console::info(
				"[ui_scripting] HKS LUI trace: %s Engine=%s(%u), "
				"Engine.GetPlayercardEmblemTableName=%s(%u), "
				"Engine.IsSteam=%s(%u), Engine.IsGDK=%s(%u), "
				"Engine.IsUbiConnect=%s(%u)\n",
				stage, hks_type_name(engine_object_type),
				static_cast<unsigned int>(engine_object_type),
				hks_type_name(function_object_type),
				static_cast<unsigned int>(function_object_type),
				hks_type_name(is_steam_object_type),
				static_cast<unsigned int>(is_steam_object_type),
				hks_type_name(is_gdk_object_type),
				static_cast<unsigned int>(is_gdk_object_type),
				hks_type_name(is_ubi_connect_object_type),
				static_cast<unsigned int>(is_ubi_connect_object_type));
		}

		bool is_lui_chunk_name(const char* chunk_name)
		{
			if (chunk_name == nullptr)
			{
				return false;
			}

			const std::string_view name(chunk_name);
			return name.find("ui/LUI/") != std::string_view::npos ||
				name.find("LUI.") != std::string_view::npos;
		}

		bool is_mp_menus_main_chunk(const char* chunk_name)
		{
			if (chunk_name == nullptr)
			{
				return false;
			}

			const std::string_view name(chunk_name);
			return name.find("mp_menus.main") != std::string_view::npos ||
				name.find("mp_menus/main") != std::string_view::npos ||
				name.find("mp_menus\\main") != std::string_view::npos;
		}

		bool is_friends_menu_chunk(const char* chunk_name)
		{
			return chunk_name != nullptr &&
				std::string_view(chunk_name).find("common_menus/FriendsMenu.lua") !=
				std::string_view::npos;
		}

		bool is_mp_zombies_movies_chunk(const char* chunk_name)
		{
			return chunk_name != nullptr &&
				std::string_view(chunk_name).find("MPZombiesMovies.lua") !=
				std::string_view::npos;
		}

		void install_lui_engine_compatibility_functions(const table& lua)
		{
			const auto engine_value = lua["Engine"];
			if (!engine_value.is<table>())
			{
				console::error(
					"[ui_scripting] Cannot register LUI Engine compatibility functions: "
					"global Engine is missing or is not a table\n");
				return;
			}

			auto engine_type = engine_value.as<table>();
			if (!engine_type["GetPlayercardEmblemTableName"].is<function>())
			{
				engine_type["GetPlayercardEmblemTableName"] = []()
				{
					constexpr auto table_name = "mp/emblemsorbackings.csv";
					if (utils::flags::has_flag("hks_lui_load_trace"))
					{
						console::info(
							"[ui_scripting] HKS LUI trace: Engine.GetPlayercardEmblemTableName "
							"shim invoked; returning '%s'\n",
							table_name);
					}

					return std::string(table_name);
				};
			}

			if (!engine_type["IsSteam"].is<function>())
			{
				engine_type["IsSteam"] = []()
				{
					return true;
				};
			}

			if (!engine_type["IsGDK"].is<function>())
			{
				engine_type["IsGDK"] = []()
				{
					return false;
				};
			}

			if (!engine_type["IsUbiConnect"].is<function>())
			{
				engine_type["IsUbiConnect"] = []()
				{
					return false;
				};
			}
		}

		bool is_cod_lua_chunk(const char* chunk_name)
		{
			return chunk_name != nullptr &&
				(std::string_view(chunk_name) == "@ui/LUI/CoD.lua" ||
					std::string_view(chunk_name) == "ui/LUI/CoD.lua");
		}

		void print_error(const std::string& error)
		{
			console::error("************** LUI script execution error **************\n");
			console::error("%s\n", error.data());
			console::error("********************************************************\n");
		}

		void print_loading_script(const std::string& name)
		{
			console::info("Loading LUI script '%s'\n", name.data());
		}

		std::string get_current_script()
		{
			const auto state = *game::hks::lua_state;
			game::hks::lua_Debug info{};
			game::hks::hksi_lua_getstack(state, 1, &info);
			game::hks::hksi_lua_getinfo(state, "nSl", &info);
			return info.short_src;
		}

		bool read_hks_diagnostic_memory(
			const std::uintptr_t address, void* output, const std::size_t size)
		{
			SIZE_T bytes_read{};
			return address != 0 &&
				ReadProcessMemory(
					GetCurrentProcess(), reinterpret_cast<const void*>(address),
					output, size, &bytes_read) != FALSE &&
				bytes_read == size;
		}

		struct hks_vm_call_snapshot
		{
			const game::hks::lua_State* state{};
			int nargs{};
			int nresults{};
			const game::hks::hksInstruction* pc{};
			const game::hks::hksInstruction* state_pc{};
			const game::hks::ActivationRecord* record{};
		};

		thread_local hks_vm_call_snapshot last_hks_vm_call{};

		void capture_hks_vm_call_snapshot(
			game::hks::lua_State* state, const int nargs, const int nresults,
			const unsigned int* pc)
		{
			last_hks_vm_call = {};
			auto& snapshot = last_hks_vm_call;
			snapshot.state = state;
			snapshot.nargs = nargs;
			snapshot.nresults = nresults;
			snapshot.pc = reinterpret_cast<const game::hks::hksInstruction*>(pc);

			if (state == nullptr)
			{
				return;
			}

			snapshot.state_pc = state->m_callStack.m_current_lua_pc;
			snapshot.record = state->m_callStack.m_current;
		}

		void hks_vm_call_internal_stub(
			game::hks::lua_State* state, const int nargs, const int nresults,
			const unsigned int* pc)
		{
			capture_hks_vm_call_snapshot(state, nargs, nresults, pc);
			hks_vm_call_internal_hook.invoke<void>(state, nargs, nresults, pc);
		}

		void append_hks_vm_call_diagnostics(
			std::string& diagnostic, const game::hks::lua_State* state,
			const hks_vm_call_snapshot& snapshot)
		{
			if (snapshot.state == nullptr)
			{
				diagnostic += "[hks] no vm_call_internal snapshot captured\n";
				return;
			}

			game::hks::ActivationRecord activation{};
			const auto activation_available = read_hks_diagnostic_memory(
				reinterpret_cast<std::uintptr_t>(snapshot.record),
				&activation, sizeof(activation));
			diagnostic += std::format(
				"[hks] last vm_call_internal state={} nargs={} nresults={} pc={} "
				"state_lua_pc={} current_record={} base={} return_address={}{}\n",
				static_cast<const void*>(snapshot.state), snapshot.nargs, snapshot.nresults,
				static_cast<const void*>(snapshot.pc),
				static_cast<const void*>(snapshot.state_pc),
				static_cast<const void*>(snapshot.record),
				activation_available
					? static_cast<const void*>(activation.m_base) : nullptr,
				activation_available
					? static_cast<const void*>(activation.m_returnAddress) : nullptr,
				snapshot.state == state ? "" : " (state mismatch)");

			const auto pc_address = reinterpret_cast<std::uintptr_t>(snapshot.pc);
			for (int offset = -2; offset <= 2; ++offset)
			{
				const auto signed_address = static_cast<std::intptr_t>(pc_address) +
					static_cast<std::intptr_t>(offset) *
					static_cast<std::intptr_t>(sizeof(std::uint32_t));
				if (pc_address == 0 || signed_address <= 0)
				{
					diagnostic += std::format(
						"[hks] vm instruction pc{:+} address=null unavailable\n", offset);
					continue;
				}

				const auto instruction_address =
					static_cast<std::uintptr_t>(signed_address);
				std::uint32_t instruction{};
				if (!read_hks_diagnostic_memory(
					instruction_address, &instruction, sizeof(instruction)))
				{
					diagnostic += std::format(
						"[hks] vm instruction pc{:+} address={} unavailable\n",
						offset, reinterpret_cast<const void*>(instruction_address));
					continue;
				}

				const auto opcode = (instruction >> 25) & 0x7F;
				const auto register_a = instruction & 0xFF;
				auto instruction_diagnostic = std::format(
					"[hks] vm instruction pc{:+} address={} word=0x{:08X} opcode={} A={}",
					offset, reinterpret_cast<const void*>(instruction_address),
					instruction, opcode, register_a);
				if (activation_available && activation.m_base != nullptr)
				{
					const auto register_address =
						reinterpret_cast<std::uintptr_t>(activation.m_base) +
						register_a * sizeof(game::hks::HksObject);
					game::hks::HksObject register_value{};
					if (read_hks_diagnostic_memory(
						register_address, &register_value, sizeof(register_value)))
					{
						instruction_diagnostic += std::format(
							" R{}={}({}) payload=0x{:016X}",
							register_a, hks_type_name(register_value.t),
							static_cast<unsigned int>(register_value.t),
							register_value.v.ui64);
					}
				}

				diagnostic += instruction_diagnostic + "\n";
			}
		}

		void append_hks_frame_diagnostics(
			std::string& diagnostic, const game::hks::lua_State* state)
		{
			const auto& call_stack = state->m_callStack;
			diagnostic += std::format(
				"[hks] VM stack records={} last_record={} current_record={} lua_pc={} "
				"hook_return={} hook_level={}\n",
				static_cast<const void*>(call_stack.m_records),
				static_cast<const void*>(call_stack.m_lastrecord),
				static_cast<const void*>(call_stack.m_current),
				static_cast<const void*>(call_stack.m_current_lua_pc),
				static_cast<const void*>(call_stack.m_hook_return_addr),
				call_stack.m_hook_level);

			game::hks::ActivationRecord active_record{};
			const auto active_record_address =
				reinterpret_cast<std::uintptr_t>(call_stack.m_current);
			const auto active_record_available = read_hks_diagnostic_memory(
				active_record_address, &active_record, sizeof(active_record));
			if (active_record_available)
			{
				diagnostic += std::format(
					"[hks] active record base={} return_address={} tail_call_depth={} "
					"varargs={} expected_returns={}\n",
					static_cast<const void*>(active_record.m_base),
					static_cast<const void*>(active_record.m_returnAddress),
					active_record.m_tailCallDepth, active_record.m_numVarargs,
					active_record.m_numExpectedReturns);

				const auto return_address =
					reinterpret_cast<std::uintptr_t>(active_record.m_returnAddress);
				if (return_address >= sizeof(std::uint32_t))
				{
					std::uint32_t return_instruction{};
					const auto instruction_address = return_address - sizeof(return_instruction);
					if (read_hks_diagnostic_memory(
						instruction_address, &return_instruction, sizeof(return_instruction)))
					{
						diagnostic += std::format(
							"[hks] preceding return instruction address={} word=0x{:08X} "
							"opcode={} A={}\n",
							reinterpret_cast<const void*>(instruction_address),
							return_instruction, (return_instruction >> 25) & 0x7F,
							return_instruction & 0xFF);
					}
				}
			}
			else
			{
				diagnostic += "[hks] active record unavailable\n";
			}

			const auto lua_pc_address =
				reinterpret_cast<std::uintptr_t>(call_stack.m_current_lua_pc);
			if (lua_pc_address == 0)
			{
				diagnostic += "[hks] current Lua PC is null\n";
				return;
			}

			for (int offset = -2; offset <= 2; ++offset)
			{
				const auto signed_address = static_cast<std::intptr_t>(lua_pc_address) +
					static_cast<std::intptr_t>(offset) *
					static_cast<std::intptr_t>(sizeof(std::uint32_t));
				if (signed_address <= 0)
				{
					continue;
				}

				const auto instruction_address = static_cast<std::uintptr_t>(signed_address);
				std::uint32_t instruction{};
				if (!read_hks_diagnostic_memory(
					instruction_address, &instruction, sizeof(instruction)))
				{
					diagnostic += std::format(
						"[hks] instruction pc{:+} address={} unavailable\n",
						offset, reinterpret_cast<const void*>(instruction_address));
					continue;
				}

				const auto opcode = (instruction >> 25) & 0x7F;
				const auto register_a = instruction & 0xFF;
				auto instruction_diagnostic = std::format(
					"[hks] instruction pc{:+} address={} word=0x{:08X} opcode={} A={}",
					offset, reinterpret_cast<const void*>(instruction_address),
					instruction, opcode, register_a);

				if (active_record_available && active_record.m_base != nullptr)
				{
					const auto register_address =
						reinterpret_cast<std::uintptr_t>(active_record.m_base) +
						register_a * sizeof(game::hks::HksObject);
					game::hks::HksObject register_value{};
					if (read_hks_diagnostic_memory(
						register_address, &register_value, sizeof(register_value)))
					{
						instruction_diagnostic += std::format(
							" R{}={}({}) payload=0x{:016X}",
							register_a, hks_type_name(register_value.t),
							static_cast<unsigned int>(register_value.t),
							register_value.v.ui64);
					}
				}

				diagnostic += instruction_diagnostic + "\n";
			}
		}

		void log_hks_error(game::hks::lua_State* state, const char* error)
		{
			const auto vm_call_snapshot = last_hks_vm_call;
			void* native_frames[32]{};
			const auto native_frame_count = CaptureStackBackTrace(0, 32, native_frames, nullptr);
			auto diagnostic = std::format("[hks] Lua error: {}\n", error);

			if (state != nullptr)
			{
				diagnostic += std::format(
					"[hks] state={} status={} prior_error=0x{:08X} callstack_level={} current_frame={}\n",
					static_cast<void*>(state), static_cast<int>(state->m_status),
					static_cast<unsigned int>(state->m_error), state->m_callStack.m_hook_level,
					static_cast<void*>(state->m_callStack.m_current));

				append_hks_frame_diagnostics(diagnostic, state);
				append_hks_vm_call_diagnostics(diagnostic, state, vm_call_snapshot);

				for (int level = 0; level < 64; ++level)
				{
					game::hks::lua_Debug info{};
					if (game::hks::hksi_lua_getstack(state, level, &info) == 0)
					{
						break;
					}

					if (game::hks::hksi_lua_getinfo(state, "nSl", &info) == 0)
					{
						diagnostic += std::format("[hks] frame[{}]: debug info unavailable\n", level);
						continue;
					}

					diagnostic += std::format(
						"[hks] frame[{}]: {}:{} type='{}' name='{}'\n",
						level, info.short_src, info.currentline,
						info.what != nullptr ? info.what : "<unknown>",
						info.name != nullptr ? info.name : "<anonymous>");
				}
			}

			for (USHORT i = 0; i < native_frame_count; ++i)
			{
				diagnostic += std::format("[hks] native_frame[{}]={}\n", i, native_frames[i]);
			}

			if (!utils::io::write_file("hks_error_diagnostics.log", diagnostic, true))
			{
				OutputDebugStringA("[ui_scripting] Failed to write hks_error_diagnostics.log\n");
			}

			OutputDebugStringA(diagnostic.c_str());
		}

		void hks_lua_error_stub(game::hks::lua_State* state, const char* format, ...)
		{
			char message[4096]{};
			va_list args;
			va_start(args, format);
			const auto format_result = std::vsnprintf(message, sizeof(message),
				format != nullptr ? format : "<null HKS error format>", args);
			va_end(args);

			if (format_result < 0)
			{
				std::snprintf(message, sizeof(message), "%s", "<failed to format HKS error message>");
			}

			log_hks_error(state, message);

			using hks_lua_error_function = void(*)(game::hks::lua_State*, const char*, ...);
			reinterpret_cast<hks_lua_error_function>(hks_lua_error_hook.get_original())(state, "%s", message);
		}

		int load_buffer(const std::string& name, const std::string& data)
		{
			const auto state = *game::hks::lua_state;
			const auto sharing_mode = state->m_global->m_bytecodeSharingMode;
			state->m_global->m_bytecodeSharingMode = game::hks::HKS_BYTECODE_SHARING_ON;
			const auto _0 = gsl::finally([&]()
			{
				state->m_global->m_bytecodeSharingMode = sharing_mode;
			});

			game::hks::HksCompilerSettings compiler_settings{};
			return game::hks::hksi_hksL_loadbuffer(state, &compiler_settings, data.data(), data.size(), name.data());
		}

		void load_script(const std::string& name, const std::string& data)
		{
			globals.loaded_scripts.push_back({name, name});

			const auto lua = get_globals();
			const auto load_results = lua["loadstring"](data, name);

			if (load_results[0].is<function>())
			{
				const auto results = lua["pcall"](load_results);
				if (!results[0].as<bool>())
				{
					print_error(results[1].as<std::string>());
				}
			}
			else if (load_results[1].is<std::string>())
			{
				print_error(load_results[1].as<std::string>());
			}
		}

		void load_scripts(const std::string& script_dir)
		{
			if (!utils::io::directory_exists(script_dir))
			{
				return;
			}

			const auto scripts = utils::io::list_files(script_dir);

			for (const auto& script : scripts)
			{
				std::string data{};
				if (std::filesystem::is_directory(script) && utils::io::read_file(script + "/__init__.lua", &data))
				{
					print_loading_script(script);
					load_script(script + "/__init__.lua", data);
				}
			}
		}

		void setup_functions()
		{
			const auto lua = get_globals();

			log_engine_status("setup_functions before registration", lua);
			install_lui_engine_compatibility_functions(lua);
			log_engine_status("setup_functions after registration", lua);

			using game = table;
			auto game_type = game();
			lua["game"] = game_type;

			game_type["getfps"] = [](const game&)
			{
				return fps::get_fps();
			};

			game_type["getping"] = [](const game&)
			{
				return *::game::mp::ping;
			};

			game_type["issingleplayer"] = [](const game&)
			{
				return ::game::environment::is_sp();
			};

			game_type["ismultiplayer"] = [](const game&)
			{
				return ::game::environment::is_mp();
			};

			game_type["addlocalizedstring"] = [](const game&, const std::string& string,
				const std::string& value)
			{
				localized_strings::override(string, value);
			};
		}

		void enable_globals()
		{
			const auto lua = get_globals();
			const std::string script =
				"local g = getmetatable(_G)\n"
				"if not g then\n"
					"g = {}\n"
					"setmetatable(_G, g)\n"
				"end\n"
				"g.__newindex = nil\n";

			(void)lua["loadstring"](script)[0]();
		}

		void start()
		{
			globals = {};

			const auto lua = get_globals();
			enable_globals();

			setup_functions();

			lua["print"] = function(reinterpret_cast<game::hks::lua_function>(0x14007CB70));

			lua["directoryexists"] = [](const std::string& string)
			{
				return utils::io::directory_exists(string);
			};

			lua["listfiles"] = [](const std::string& string)
			{
				return utils::io::list_files(string);
			};

			lua["directoryisempty"] = [](const std::string& string)
			{
				return utils::io::directory_is_empty(string);
			};

			lua["fileexists"] = [](const std::string& string)
			{
				return utils::io::file_exists(string);
			};

			lua["table"]["unpack"] = lua["unpack"];
			lua["luiglobals"] = lua;

			load_scripts(game_module::get_host_module().get_folder() + "/data/ui_scripts/");
			load_scripts("s1/ui_scripts/");
		}

		void try_start()
		{
			try
			{
				start();
			}
			catch (const std::exception& e)
			{
				console::error("Failed to load LUI scripts: %s\n", e.what());
			}

			running = *game::hks::lua_state != nullptr;
		}

		void* hks_start_stub(char a1)
		{
			const auto _0 = gsl::finally(&try_start);
			return hks_start_hook.invoke<void*>(a1);
		}

		void hks_shutdown_stub()
		{
			running = false;
			converted_functions.clear();
			globals = {};
			return hks_shutdown_hook.invoke<void>();
		}

		void* hks_package_require_stub(game::hks::lua_State* state)
		{
			const auto script = get_current_script();
			const auto root = get_root_script(script);
			globals.in_require_script = root;
			return hks_package_require_hook.invoke<void*>(state);
		}

		game::XAssetHeader db_find_x_asset_header_stub(game::XAssetType type, const char* name, int allow_create_default)
		{
			game::XAssetHeader header{.luaFile = nullptr};

			if (!is_loaded_script(globals.in_require_script))
			{
				return game::DB_FindXAssetHeader(type, name, allow_create_default);
			}

			const auto folder = globals.in_require_script.substr(0, globals.in_require_script.find_last_of("/\\"));
			const std::string name_ = name;
			const std::string target_script = folder + "/" + name_ + ".lua";

			if (utils::io::file_exists(target_script))
			{
				globals.load_raw_script = true;
				globals.raw_script_name = target_script;
				header.luaFile = reinterpret_cast<game::LuaFile*>(1);
			}
			else if (name_.starts_with("ui/LUI/"))
			{
				return game::DB_FindXAssetHeader(type, name, allow_create_default);
			}

			return header;
		}

		using hks_reader_function = unsigned char* (__fastcall*)(
			game::hks::lua_State*, void*, std::size_t*);

		// Preserve the HKS reader's result and user data while teeing each returned chunk.
		struct hks_reader_capture
		{
			hks_reader_function reader;
			void* reader_data;
			std::FILE* dump_file;
			std::size_t captured_bytes{};
			bool write_failed{};
		};

		unsigned char* __fastcall hks_reader_capture_stub(
			game::hks::lua_State* state, void* reader_data, std::size_t* size)
		{
			auto* const capture = static_cast<hks_reader_capture*>(reader_data);
			auto* const chunk = capture->reader(state, capture->reader_data, size);
			if (chunk != nullptr && size != nullptr && *size != 0 && !capture->write_failed)
			{
				const auto written = std::fwrite(chunk, 1, *size, capture->dump_file);
				if (written != *size)
				{
					capture->write_failed = true;
				}
				else
				{
					capture->captured_bytes += written;
				}
			}

			return chunk;
		}

		void log_hks_load_result(const bool trace, const char* chunk_name, const int result)
		{
			if (trace)
			{
				console::info(
					"[ui_scripting] HKS LUI trace: load exit chunk='%s' result=%d\n",
					chunk_name != nullptr ? chunk_name : "<null>", result);
			}
		}

		int hks_load_stub(game::hks::lua_State* state, void* compiler_options, 
			void* reader, void* reader_data, const char* chunk_name)
		{
			const auto trace_lui_load = utils::flags::has_flag("hks_lui_load_trace") &&
				is_lui_chunk_name(chunk_name);
			if (trace_lui_load)
			{
				console::info(
					"[ui_scripting] HKS LUI trace: load enter chunk='%s' state=%p\n",
					chunk_name != nullptr ? chunk_name : "<null>", static_cast<void*>(state));
				log_engine_status(
					"load entry", table{state->globals.v.table});
			}

			if (is_cod_lua_chunk(chunk_name))
			{
				install_lui_engine_compatibility_functions(
					table{state->globals.v.table});
				log_engine_status(
					"after CoD.lua compatibility registration",
					table{state->globals.v.table});
			}

			if (globals.load_raw_script)
			{
				globals.load_raw_script = false;
				globals.loaded_scripts.push_back({globals.raw_script_name, globals.in_require_script});
				const auto result = load_buffer(
					globals.raw_script_name, utils::io::read_file(globals.raw_script_name));
				log_hks_load_result(trace_lui_load, chunk_name, result);
				return result;
			}

			const auto capture_cod_chunk = utils::flags::has_flag("hks_cod_reader_dump") &&
				is_cod_lua_chunk(chunk_name);
			const auto capture_mp_menus_main = utils::flags::has_flag("hks_lui_load_trace") &&
				is_mp_menus_main_chunk(chunk_name);
			const auto capture_friends_menu = utils::flags::has_flag("hks_lui_load_trace") &&
				is_friends_menu_chunk(chunk_name);
			const auto capture_mp_zombies_movies = utils::flags::has_flag("hks_lui_load_trace") &&
				is_mp_zombies_movies_chunk(chunk_name);
			if (capture_cod_chunk || capture_mp_menus_main || capture_friends_menu ||
				capture_mp_zombies_movies)
			{
				const auto* const dump_path = capture_cod_chunk
					? "hks_cod_lua_reader.bin"
					: capture_mp_menus_main
					? "hks_lui_mp_menus_main_reader.bin"
					: capture_friends_menu
					? "hks_lui_friendsmenu_reader.bin"
					: "hks_lui_mp_zombies_movies_reader.bin";
				std::FILE* dump_file{};
				const auto open_result = ::fopen_s(&dump_file, dump_path, "wb");
				if (open_result != 0 || dump_file == nullptr)
				{
					console::error(
						"[ui_scripting] Failed to open '%s' for the HKS reader dump (error=%d)\n",
						dump_path, open_result);
				}
				else if (reader == nullptr)
				{
					const auto close_result = std::fclose(dump_file);
					console::error(
						"[ui_scripting] HKS reader for '%s' is null; using the original loader path "
						"(dump_close_failed=%d)\n",
						chunk_name, close_result != 0 ? 1 : 0);
				}
				else
				{
					auto capture = hks_reader_capture{
						reinterpret_cast<hks_reader_function>(reader),
						reader_data,
						dump_file,
					};
					const auto result = utils::hook::invoke<int>(
						0x14009CA10, state, compiler_options, &hks_reader_capture_stub, &capture, chunk_name);
					const auto close_result = std::fclose(dump_file);

					if (capture.write_failed || close_result != 0)
					{
						console::error(
							"[ui_scripting] HKS reader dump for '%s' failed after %llu bytes "
							"(reader_write_failed=%d, close_failed=%d, loader_result=%d)\n",
							chunk_name,
							static_cast<unsigned long long>(capture.captured_bytes),
							capture.write_failed ? 1 : 0, close_result != 0 ? 1 : 0, result);
					}
					else
					{
						console::info(
							"[ui_scripting] Captured %llu bytes from the stock HKS reader for '%s' "
							"to '%s' (loader_result=%d)\n",
							static_cast<unsigned long long>(capture.captured_bytes),
							chunk_name, dump_path, result);
					}

					log_hks_load_result(trace_lui_load, chunk_name, result);
					return result;
				}
			}

			const auto result = utils::hook::invoke<int>(
				0x14009CA10, state, compiler_options, reader, reader_data, chunk_name);
			log_hks_load_result(trace_lui_load, chunk_name, result);
			return result;
		}

		int main_handler(game::hks::lua_State* state)
		{
			const auto value = state->m_apistack.base[-1];
			if (value.t != game::hks::TCFUNCTION)
			{
				return 0;
			}

			const auto closure = value.v.cClosure;
			if (!converted_functions.contains(closure))
			{
				return 0;
			}

			const auto& function = converted_functions[closure];

			try
			{
				const auto args = get_return_values();
				const auto results = function(args);

				for (const auto& result : results)
				{
					push_value(result);
				}

				return static_cast<int>(results.size());
			}
			catch (const std::exception& e)
			{
				game::hks::hksi_luaL_error(state, e.what());
			}

			return 0;
		}
	}

	bool lui_running()
	{
		return running && *game::hks::lua_state != nullptr;
	}

	template <typename F>
	game::hks::cclosure* convert_function(F f)
	{
		const auto state = *game::hks::lua_state;
		const auto closure = game::hks::cclosure_Create(state, main_handler, 0, 0, 0);
		converted_functions[closure] = wrap_function(f);
		return closure;
	}

	class component final : public component_interface
	{
	public:

		void post_unpack() override
		{
			if (!game::environment::is_mp())
			{
				return;
			}

			if (utils::flags::has_flag("hks_error_diagnostics"))
			{
				hks_lua_error_hook.create(game::hks::hksi_luaL_error, &hks_lua_error_stub);
				hks_vm_call_internal_hook.create(
					game::hks::vm_call_internal, &hks_vm_call_internal_stub);
				console::info(
					"[ui_scripting] HKS error diagnostics enabled; stack traces will be written to "
					"hks_error_diagnostics.log with the last VM callsite\n");
			}

			if (utils::flags::has_flag("hks_cod_reader_dump") &&
				!utils::flags::has_flag("no_hks_ui_hooks"))
			{
				console::info(
					"[ui_scripting] HKS reader capture enabled for @ui/LUI/CoD.lua; "
					"data will be written to hks_cod_lua_reader.bin\n");
			}

			if (utils::flags::has_flag("no_hks_ui_hooks"))
			{
				console::info(
					"HKS UI hooks bypassed by -no_hks_ui_hooks: DB-find "
					"(0x1400CB68B, 0x1400CB7D6), HKS-load (0x1400CB861), package-require "
					"(0x1400781D0), start (0x1400E4E40), shutdown (0x1400DB9A0). "
					"No hooks in this set were installed.\n");
				return;
			}

			if (utils::flags::has_flag("hks_lui_load_trace"))
			{
				console::info(
					"[ui_scripting] HKS LUI load trace enabled; matching chunk names, helper state, "
					"loader results, and the LUI.mp_menus.main reader will be logged/captured\n");
			}

			utils::hook::call(0x1400CB68B, db_find_x_asset_header_stub);
			utils::hook::call(0x1400CB7D6, db_find_x_asset_header_stub);
			utils::hook::call(0x1400CB861, hks_load_stub);

			hks_package_require_hook.create(0x1400781D0, hks_package_require_stub);
			hks_start_hook.create(0x1400E4E40, hks_start_stub);
			hks_shutdown_hook.create(0x1400DB9A0, hks_shutdown_stub);

			command::add("lui_restart", []
			{
				utils::hook::invoke<void>(0x1400DB9A0);
				utils::hook::invoke<void>(0x1400E6730);
			});
		}
	};
}

REGISTER_COMPONENT(ui_scripting::component)
