#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/dvars.hpp"

#include "fastfiles.hpp"
#include "command.hpp"
#include "console.hpp"

#include <utils/concurrency.hpp>
#include <utils/flags.hpp>
#include <utils/hook.hpp>
#include <utils/io.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace fastfiles
{
	static utils::concurrency::container<std::string> current_fastfile;

	namespace
	{
		utils::hook::detour db_try_load_x_file_internal_hook;
		utils::hook::detour db_find_x_asset_header_hook;
		utils::hook::detour db_read_stream_file_hook;
		utils::hook::detour sys_error_hook;

		void capture_font_pool_error(const char* error)
		{
			const auto type = game::ASSET_TYPE_FONT;
			const auto capacity = game::g_poolSize[type];
			const auto* const pool = static_cast<const game::Font_s*>(game::DB_XAssetPool[type]);
			const auto fastfile = get_current_fastfile();
			auto diagnostic = std::format(
				"[fastfiles] Font-pool limit error: {}\n"
				"[fastfiles] fastfile='{}' pool={} capacity={} element_size={} bytes\n",
				error, fastfile, static_cast<const void*>(pool), capacity, sizeof(game::Font_s));

			if (pool != nullptr && capacity >= 0)
			{
				constexpr auto max_entries_to_log = 256;
				const auto entry_count = std::min(capacity, max_entries_to_log);
				int used = 0;
				for (auto i = 0; i < entry_count; ++i)
				{
					used += pool[i].fontName != nullptr && pool[i].fontName[0] != '\0';
				}

				diagnostic += std::format(
					"[fastfiles] registered fonts in first {} pool entries: {}\n", entry_count, used);

				for (auto i = 0; i < entry_count; ++i)
				{
					const auto* const name = pool[i].fontName;
					if (name != nullptr && name[0] != '\0')
					{
						diagnostic += std::format("[fastfiles] font[{}]='{}'\n", i, name);
					}
				}

				if (entry_count < capacity)
				{
					diagnostic += std::format("[fastfiles] listed first {} pool entries only\n", entry_count);
				}
			}
			else
			{
				diagnostic += "[fastfiles] font-pool entries unavailable\n";
			}

			if (!utils::io::write_file("font_pool_diagnostics.log", diagnostic, true))
			{
				OutputDebugStringA("[fastfiles] Failed to write font_pool_diagnostics.log\n");
				OutputDebugStringA(diagnostic.c_str());
			}
			else
			{
				OutputDebugStringA(diagnostic.c_str());
			}
		}

		void sys_error_stub(const char* format, ...)
		{
			char message[4096]{};
			va_list args;
			va_start(args, format);
			const auto format_result = std::vsnprintf(message, sizeof(message),
				format != nullptr ? format : "<null Sys_Error format>", args);
			va_end(args);

			if (format_result < 0)
			{
				std::snprintf(message, sizeof(message), "%s", "<failed to format Sys_Error message>");
			}

			if (std::strstr(message, "limit") != nullptr && std::strstr(message, "font") != nullptr)
			{
				capture_font_pool_error(message);
			}

			using sys_error_function = void(*)(const char*, ...);
			reinterpret_cast<sys_error_function>(sys_error_hook.get_original())("%s", message);
		}

		int db_read_stream_file_stub(int allow_abort, int finish)
		{
			// always use lz4 compressor type when reading stream files
			*game::g_compressor = game::DB_COMPRESSOR_LZX;
			return db_read_stream_file_hook.invoke<int>(allow_abort, finish);
		}

		void db_try_load_x_file_internal(const char* zone_name, const int flags)
		{
			console::info("Loading fastfile %s\n", zone_name);
			current_fastfile.access([&](std::string& fastfile)
			{
				fastfile = zone_name;
			});
			return db_try_load_x_file_internal_hook.invoke<void>(zone_name, flags);
		}

		void dump_gsc_script(const std::string& name, game::XAssetHeader header)
		{
			if (!dvars::g_dump_scripts->current.enabled)
			{
				return;
			}

			std::string buffer;
			buffer.append(header.scriptfile->name, std::strlen(header.scriptfile->name) + 1);
			buffer.append(reinterpret_cast<char*>(&header.scriptfile->compressedLen), sizeof(int));
			buffer.append(reinterpret_cast<char*>(&header.scriptfile->len), sizeof(int));
			buffer.append(reinterpret_cast<char*>(&header.scriptfile->bytecodeLen), sizeof(int));
			buffer.append(header.scriptfile->buffer, header.scriptfile->compressedLen);
			buffer.append(reinterpret_cast<char*>(header.scriptfile->bytecode), header.scriptfile->bytecodeLen);

			const auto out_name = std::format("gsc_dump/{}.gscbin", name);
			utils::io::write_file(out_name, buffer);

			console::info("Dumped %s\n", out_name.data());
		}

		game::XAssetHeader db_find_x_asset_header_stub(game::XAssetType type, const char* name, int allow_create_default)
		{
			const auto start = game::Sys_Milliseconds();
			const auto result = db_find_x_asset_header_hook.invoke<game::XAssetHeader>(type, name, allow_create_default);
			const auto diff = game::Sys_Milliseconds() - start;

			if (type == game::ASSET_TYPE_SCRIPTFILE)
			{
				dump_gsc_script(name, result);
			}

			if (diff > 100)
			{
				console::print(
					result.data == nullptr ? console::con_type_error : console::con_type_warning, "Waited %i msec for asset '%s' of type '%s'.\n",
					diff,
					name,
					game::g_assetNames[type]
				);
			}

			return result;
		}
	}

	std::string get_current_fastfile()
	{
		auto fastfile_copy = current_fastfile.access<std::string>([&](std::string& fastfile)
		{
			return fastfile;
		});

		return fastfile_copy;
	}

	constexpr int get_asset_type_size(const game::XAssetType type)
	{
		constexpr int asset_type_sizes[] =
		{
			96, 88, 128, 56, 40, 216, 56, 680,
			480, 32, 32, 32, 32, 32, 352, 1456,
			104, 32, 24, 152, 152, 152, 16, 64,
			640, 40, 16, 408, 24, 288, 176, 2800,
			48, -1, 40, 24, 200, 88, 16, 120,
			3560, 32, 64, 16, 16, -1, -1, -1,
			-1, 24, 40, 24, 40, 24, 128, 2256,
			136, 32, 72, 24, 64, 88, 48, 32,
			96, 152, 64, 32,
		};

		return asset_type_sizes[type];
	}

	template <game::XAssetType Type, size_t Size>
	char* reallocate_asset_pool()
	{
		constexpr auto element_size = get_asset_type_size(Type);
		static char new_pool[element_size * Size]{0};
		assert(get_asset_type_size(Type) == game::DB_GetXAssetTypeSize(Type));

		std::memmove(new_pool, game::DB_XAssetPool[Type], game::g_poolSize[Type] * element_size);

		game::DB_XAssetPool[Type] = new_pool;
		game::g_poolSize[Type] = Size;

		return new_pool;
	}

	void enum_assets(const game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, const bool include_override)
	{
		game::DB_EnumXAssets_Internal(type, static_cast<void(*)(game::XAssetHeader, void*)>([](game::XAssetHeader header, void* data)
		{
			const auto& cb = *static_cast<const std::function<void(game::XAssetHeader)>*>(data);
			cb(header);
		}), &callback, include_override);
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (utils::flags::has_flag("font_pool_diagnostics"))
			{
				sys_error_hook.create(game::Sys_Error, &sys_error_stub);
				console::info(
					"[fastfiles] Font-pool diagnostics enabled; overflow details will be written to "
					"font_pool_diagnostics.log\n");
			}

			db_try_load_x_file_internal_hook.create(SELECT_VALUE(0x1401816F0, 0x1402741C0), &db_try_load_x_file_internal);

			db_find_x_asset_header_hook.create(game::DB_FindXAssetHeader, db_find_x_asset_header_stub);
			dvars::g_dump_scripts = game::Dvar_RegisterBool("g_dumpScripts", false, game::DVAR_FLAG_NONE);

			command::add("g_poolSizes", []()
			{
				for (auto i = 0; i < game::ASSET_TYPE_COUNT; i++)
				{
					console::info("g_poolSize[%i]: %i // %s\n", i, game::g_poolSize[i], game::g_assetNames[i]);
				}
			});

			// Allow loading of mixed compressor types
			utils::hook::nop(SELECT_VALUE(0x1401536D7, 0x140242DF7), 2);

			reallocate_asset_pool<game::ASSET_TYPE_FONT, 48>();

			if (!game::environment::is_sp())
			{
				const auto* xmodel_pool = reallocate_asset_pool<game::ASSET_TYPE_XMODEL, 8832>();
				utils::hook::inject(0x14026FD63, xmodel_pool + 8);
				utils::hook::inject(0x14026FDB3, xmodel_pool + 8);
				utils::hook::inject(0x14026FFAC, xmodel_pool + 8);
				utils::hook::inject(0x14027463C, xmodel_pool + 8);
				utils::hook::inject(0x140274689, xmodel_pool + 8);

				// Reallocate asset pools
				// Disabled because it causes a crash in the main menu once you rejoin a server after
				// disconnecting and waiting for the server to map rotating.
#if 0
				reallocate_asset_pool<game::ASSET_TYPE_LUA_FILE, 768>();
				reallocate_asset_pool<game::ASSET_TYPE_WEAPON, 1400>();
				reallocate_asset_pool<game::ASSET_TYPE_LOCALIZE_ENTRY, 27200>();
				reallocate_asset_pool<game::ASSET_TYPE_XANIMPARTS, 11600>();
				reallocate_asset_pool<game::ASSET_TYPE_ATTACHMENT, 256>();
				reallocate_asset_pool<game::ASSET_TYPE_FONT, 96>();
				reallocate_asset_pool<game::ASSET_TYPE_SNDDRIVER_GLOBALS, 4>();
				reallocate_asset_pool<game::ASSET_TYPE_EQUIPMENT_SND_TABLE, 4>();
				reallocate_asset_pool<game::ASSET_TYPE_SOUND, 32000>();
				reallocate_asset_pool<game::ASSET_TYPE_LOADED_SOUND, 14000>();
				reallocate_asset_pool<game::ASSET_TYPE_VERTEXDECL, 3072>();
				reallocate_asset_pool<game::ASSET_TYPE_COMPUTESHADER, 1024>();
				reallocate_asset_pool<game::ASSET_TYPE_REVERB_PRESET, 128>();
				reallocate_asset_pool<game::ASSET_TYPE_IMPACT_FX, 40>();
#endif
				// Fix compressor type on streamed file load
				db_read_stream_file_hook.create(0x14027AA70, db_read_stream_file_stub);

				// Allow loading of unsigned fastfiles
				utils::hook::nop(0x1402427A5, 2); // DB_InflateInit
			}
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
