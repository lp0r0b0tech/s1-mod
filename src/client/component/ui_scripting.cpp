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

		void install_emblem_table_name_function(const table& lua)
		{
			const auto engine_value = lua["Engine"];
			if (!engine_value.is<table>())
			{
				console::error(
					"[ui_scripting] Cannot register Engine.GetPlayercardEmblemTableName: "
					"global Engine is missing or is not a table\n");
				return;
			}

			auto engine_type = engine_value.as<table>();
			if (!engine_type["GetPlayercardEmblemTableName"].is<function>())
			{
				engine_type["GetPlayercardEmblemTableName"] = []()
				{
					return std::string("mp/emblemsorbackings.csv");
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

		void log_hks_error(game::hks::lua_State* state, const char* error)
		{
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

				for (int level = 1; level < 64; ++level)
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

			install_emblem_table_name_function(lua);

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

		int hks_load_stub(game::hks::lua_State* state, void* compiler_options, 
			void* reader, void* reader_data, const char* chunk_name)
		{
			if (is_cod_lua_chunk(chunk_name))
			{
				install_emblem_table_name_function(table{state->globals.v.table});
			}

			if (globals.load_raw_script)
			{
				globals.load_raw_script = false;
				globals.loaded_scripts.push_back({globals.raw_script_name, globals.in_require_script});
				return load_buffer(globals.raw_script_name, utils::io::read_file(globals.raw_script_name));
			}

			if (utils::flags::has_flag("hks_cod_reader_dump") && is_cod_lua_chunk(chunk_name))
			{
				constexpr auto dump_path = "hks_cod_lua_reader.bin";
				std::FILE* dump_file{};
				const auto open_result = ::fopen_s(&dump_file, dump_path, "wb");
				if (open_result != 0 || dump_file == nullptr)
				{
					console::error(
						"[ui_scripting] Failed to open '%s' for the HKS CoD.lua reader dump (error=%d)\n",
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
							"[ui_scripting] HKS CoD.lua reader dump failed after %llu bytes "
							"(reader_write_failed=%d, close_failed=%d, loader_result=%d)\n",
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

					return result;
				}
			}

			return utils::hook::invoke<int>(0x14009CA10, state, compiler_options, reader, reader_data, chunk_name);
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
				console::info(
					"[ui_scripting] HKS error diagnostics enabled; stack traces will be written to "
					"hks_error_diagnostics.log\n");
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
