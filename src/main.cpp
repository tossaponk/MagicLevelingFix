namespace
{
	void InitializeLog()
	{
#ifndef NDEBUG
		auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
		auto path = logger::log_directory();
		if (!path) {
			util::report_and_fail("Failed to find standard logging directory"sv);
		}

		*path /= fmt::format("{}.log"sv, Plugin::NAME);
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

#ifndef NDEBUG
		const auto level = spdlog::level::trace;
#else
		const auto level = spdlog::level::info;
#endif

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(level);
		log->flush_on(level);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);
	}
}

#ifdef SKYRIM_SUPPORT_AE
extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() 
{
	SKSE::PluginVersionData v;

	v.PluginVersion(Plugin::VERSION);
	v.PluginName(Plugin::NAME);

	v.UsesAddressLibrary(true);
	//v.CompatibleVersions({ SKSE::RUNTIME_LATEST });

	return v;
}();
#endif

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	(void)a_info;

	InitializeLog();

	logger::info( "{} v{}"sv, Plugin::NAME, Plugin::VERSION.string() );

	if (a_skse->IsEditor()) 
	{
		logger::critical( "Not supported on editor."sv );
		return false;
	}

	const auto ver = a_skse->RuntimeVersion();
	if( ver < SKSE::RUNTIME_1_5_97 )
	{
		logger::critical( "Not supported on version older than 1.5.97."sv );
		return false;
	}

	a_info->name = Plugin::NAME.data();
	a_info->version = 1;

	return true;
}

static uint64_t g_reportInterval = 1;
static bool g_ignoreMultiplier = false;
static bool g_reportEXPGain = true;
static float g_reportMin = 10;
namespace MagicLevelingFix
{

	static std::map<RE::ActorValue,std::string> g_AVStringMap
	{
		{ RE::ActorValue::kOneHanded,		"One-Handed" },
		{ RE::ActorValue::kTwoHanded,		"Two-Handed" },
		{ RE::ActorValue::kArchery,			"Archery" },
		{ RE::ActorValue::kBlock,			"Block" },
		{ RE::ActorValue::kSmithing,		"Smithing" },
		{ RE::ActorValue::kHeavyArmor,		"HeavyArmor" },
		{ RE::ActorValue::kLightArmor,		"LightArmor" },
		{ RE::ActorValue::kPickpocket,		"Pickpocket" },
		{ RE::ActorValue::kLockpicking,		"Lockpicking" },
		{ RE::ActorValue::kSneak,			"Sneak" },
		{ RE::ActorValue::kAlchemy,			"Alchemy" },
		{ RE::ActorValue::kSpeech,			"Speech" },
		{ RE::ActorValue::kAlteration,		"Alteration" },
		{ RE::ActorValue::kConjuration,		"Conjuration" },
		{ RE::ActorValue::kDestruction,		"Destruction" },
		{ RE::ActorValue::kIllusion,		"Illusion" },
		{ RE::ActorValue::kRestoration,		"Restoration" },
		{ RE::ActorValue::kEnchanting,		"Enchanting" },
	};

	struct SkillEXPHook
	{
		struct ASMPatch : public Xbyak::CodeGenerator
		{
			ASMPatch( size_t a_retAddr )
			{
				Xbyak::Label callLabel;
				Xbyak::Label retLabel;

				push( rcx );
				push( rdx );
				sub( rsp, 8 );
				movss( ptr[ rsp ], xmm2 );

				sub( rsp, 0x20 );
				call( ptr[ rip + callLabel ] );

				add( rsp, 0x20 );

				movss( xmm2, ptr[ rsp ] );
				add( rsp, 8 );
				pop( rdx );
				pop( rcx );

				sub( rsp, 0x48 );
				xorps( xmm0, xmm0 );

				jmp( ptr[ rip + retLabel ] );

				L( callLabel );
				dq( (size_t)&thunk );

				L( retLabel );
				dq( a_retAddr );

				ready();
			}
		};

		static void thunk( RE::PlayerCharacter* a_this, RE::ActorValue a_skill, float a_experience )
		{
			if( a_experience == 0 )
				return;

			_CRT_UNUSED( a_this );

			using clock = std::chrono::system_clock;
			using time = clock::time_point;
			struct ExpInfo
			{
				time	lastReport;
				float	fAccumulatedEXP;
			};
			static std::map<RE::ActorValue,ExpInfo> expInfoMap;

			auto& expInfo = expInfoMap[ a_skill ];
			expInfo.fAccumulatedEXP += a_experience;

			auto now = clock::now();
			std::chrono::duration<float> elapsed = now - expInfo.lastReport;
			if( (elapsed >= std::chrono::seconds( g_reportInterval ) && expInfo.fAccumulatedEXP > 0) ||
				a_experience >= g_reportMin )
			{
				RE::ConsoleLog::GetSingleton()->Print( "Skill %s EXP gained: %f", g_AVStringMap[ a_skill ].c_str(), expInfo.fAccumulatedEXP );
				expInfo.fAccumulatedEXP = 0;
				expInfo.lastReport = now;
			}
		}

		static void Install()
		{
			REL::Relocation<uintptr_t> reloc( RELOCATION_ID(39413, 40488) );
			ASMPatch patch( reloc.address() + 7 );
			REL::safe_fill( reloc.address(), 0x90, 7 );

			stl::write_patch_branch<SkillEXPHook>( reloc.address(), patch );
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SpellUsageHook
	{
		static bool thunk( RE::SpellItem* a_this, RE::SpellItem::SkillUsageData& a_skillUsage )
		{
			bool shouldRewardEXP = func( a_this, a_skillUsage );
			if( shouldRewardEXP && !a_this->IsAutoCalc() )
			{
				float realCost	= a_this->CalculateMagickaCost( nullptr );
				auto mainEffect	= a_this->GetCostliestEffectItem();
				if( realCost > 0 && mainEffect )
				{
					if( mainEffect->baseEffect->data.skillUsageMult != 0 )
					{
						if( g_ignoreMultiplier )
							a_skillUsage.magnitude = realCost;
						else
						{
							float expReward = realCost * mainEffect->baseEffect->data.skillUsageMult;

							// Reward EXP must not exceed spell cost to prevent human error when assigning multiplier
							// Because multiplier higher than 1 is usually the hacky way author used to work around auto calc problem
							a_skillUsage.magnitude = min( expReward, realCost );
						}
					}
				}
			}
			
			return shouldRewardEXP;
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static inline size_t size = 0x60;

		static void Install()
		{
			stl::write_vfunc<RE::SpellItem, SpellUsageHook>();
		}
	};
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load( const SKSE::LoadInterface* a_skse )
{
	InitializeLog();

	SKSE::Init( a_skse );

	CSimpleIniA iniFile;
	iniFile.SetUnicode();
	iniFile.SetMultiKey();
	iniFile.LoadFile( L"Data/SKSE/Plugins/MagicLevelingFix.ini" );

	g_ignoreMultiplier	= iniFile.GetBoolValue( "Settings", "IgnoreSkillUseMult", false );
	g_reportEXPGain		= iniFile.GetBoolValue( "Settings", "ReportEXPGain", false );
	g_reportInterval	= iniFile.GetLongValue( "Settings", "ReportInterval", 1 );
	g_reportMin			= (float)iniFile.GetDoubleValue( "Settings", "ReportMinimum", 10 );

	// Only install exp reporting hook when enabled
	if( g_reportEXPGain )
		MagicLevelingFix::SkillEXPHook::Install();

	MagicLevelingFix::SpellUsageHook::Install();

	return true;
}

