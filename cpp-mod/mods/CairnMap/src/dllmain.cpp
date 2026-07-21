// CairnMap — map collectables for Palworld 1.0+ (see docs/SPEC-2.0.md)
// P1: static layers rendered as tinted dots in our own canvas, positioned by
// the boss-tower-anchored projection (cairn_project.hpp, unit-tested natively).
// ABI target: UE4SS v3.0.1 (Okaetsu experimental-palworld, c2ac246, MSVC).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOMINMAX: Windows.h defines min/max as macros, which breaks std::max inside
// UE4SS's UnrealString.hpp. WIN32_LEAN_AND_MEAN drops winsock/ole/rpc headers we
// have no use for. Windows.h is here solely for GetModuleFileNameW, to locate the
// mod's own directory for settings.txt.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DynamicOutput/Output.hpp>
#include <Input/Handler.hpp>
#include <Input/KeyDef.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSRuntime.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/FAssetData.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/UStruct.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "cairn_data.hpp"
#include "cairn_relic_types.hpp"
#include "cairn_project.hpp"

namespace CairnMap
{
    using namespace RC;
    using namespace RC::Unreal;

    // Dot sizing tunables, overridable from settings.txt so they can be dialled in
    // without a CI round-trip. See Mod::dot_render_scale for what they mean.
    inline double g_dot_scale = 1.0;   // user multiplier on the base sizes below
    inline double g_dot_growth = 0.0;  // px = base * zoom^growth; 0 = same size at every zoom

    // Dev only: save directory to auto-load from the title screen (Mod::try_autoload).
    // Empty = off, and it stays empty unless settings.txt names a world -- a mod that
    // loads a save on its own is a bug to everyone except us.
    inline std::wstring g_autoload_world;
// The ore compass is OFF until it can place an icon.
//
// Everything about it works except the one thing that makes it a compass: the
// icons register, resolve to real rocks, carry vanilla's own per-ore art and the
// right distance -- and all land stacked at dead centre instead of at their
// bearing. Shipping that reads as a broken mod, not a beta feature, so it is
// opt-in until the placement is solved. See docs and Mod::update_ore_compass.
inline bool g_ore_compass = false;

// Native minimap (#26), replacing the ore compass: an always-on HUD widget we
// build ourselves, so there is no game widget to fight for icon placement (the
// compass's fatal problem). This is the SPIKE gate -- it draws only the frame
// (a square, top-right, on WBP_PlayerUI's CanvasPanel_Root) so the one real
// unknown, "can we put a persistent widget on the gameplay HUD", is proven
// before the coordinate transform and dots go in. Off by default until then.
inline bool g_minimap = false;
// Minimap placement on a 3x3 grid so it can dodge any HUD element. ax/ay each
// take 0 (left/top), 0.5 (center) or 1 (right/bottom); settings.txt names them
// with a two-letter code, vertical then horizontal (TL TC TR CL CC CR BL BC BR).
// Margins push it in from an edge and are ignored on a centered axis. Default
// CL (center-left), clear of the top-right quest tracker.
inline double g_minimap_ax = 0.0;   // horizontal: 0 left, 0.5 centre, 1 right
inline double g_minimap_ay = 0.5;   // vertical:   0 top,  0.5 centre, 1 bottom
inline double g_minimap_ox = 14.0;
inline double g_minimap_oy = 14.0;
inline double g_minimap_px = 220.0;      // square edge in screen px (MinimapSize)
inline double g_minimap_range_uu = 6000.0;   // half-width shown, world units (MinimapRange m * 100); 60 m
// EXPERIMENTAL terrain (MinimapTerrain): spawns a top-down SceneCapture2D whose
// render target is the minimap background -- live world geometry, like PalMiniMap.
// Opt-in: it spawns an actor and renders the scene a second time each frame (GPU
// cost), and a wrong ProcessEvent layout on the spawn path could crash. Off until
// proven.
inline bool g_minimap_terrain = false;
// Terrain capture tuning -- sweepable via settings.txt while diagnosing the
// flat-yellow render, so combos can be tried without a rebuild. Height above the
// player (uu) trades atmospheric-fog washout (high) against clipping local relief
// (low). Source is the ESceneCaptureSource: 2 = FinalColorLDR (tonemapped, includes
// fog), 0 = SceneColorHDR, 7 = BaseColor (unlit albedo -- no fog/lighting, the
// leading fix for the yellow haze).
inline double g_terrain_height = 12000.0;   // 120 m
inline int g_terrain_source = 7;            // SCS_BaseColor
// Lit source (FinalColorLDR=2) keeps emissive POI icons coloured (BaseColor=7 shows
// albedo only -> those icons go black), but the bright top-down view can blow out.
// Auto-exposure ADAPT was tried (let night brighten, day darken) but never fired: a
// scene capture's eye adaptation needs the EyeAdaptation showflag, OFF by default on
// captures, so it stayed dark. So the capture is forced to MANUAL exposure and this is
// a direct brightness dial in EV stops: 0 = neutral, positive = brighter, negative =
// darker. Tune one value so both day and night read (night black -> raise it).
inline double g_terrain_exposure = 1.0;
// Rotate the map with the player (player-forward = up, "you" arrow fixed pointing
// up) vs north-up (map fixed, arrow rotates to your facing). Default ROTATE: once
// the rotation sign was fixed (90 + yaw -- the game's yaw is clockwise), Kenny asked
// for the arrow pinned up and the map spinning under it. F6 toggles to north-up.
inline bool g_minimap_rotate = true;

// Compass strip (#24 revival). A thin horizontal HUD bar that shows the nearest
// uncollected node of every enabled layer at its CAMERA-RELATIVE bearing --
// centre = straight ahead, left/right = to your sides -- with vanilla's own item
// art and a distance readout. Unlike the native ore compass (which builds our
// icons but never aims them -- render(0,0), a proven dead end), we draw the strip
// ourselves so placement is entirely ours. Default ON: this is the nav tool now.
inline bool g_compass = true;
inline double g_compass_w = 720.0;    // strip width in screen px (CompassWidth)
inline double g_compass_h = 44.0;     // strip height in screen px (CompassHeight)
inline double g_compass_arc = 90.0;   // half-arc in degrees shown each side of centre (CompassArc)
// Degrees added to the camera forward bearing before placing markers (CompassYawOffset).
// The whole strip rotates by this: 180 flips front<->back, ±90 rotates a quarter. Default 0
// = markers centre on what you face. Exposed so a wrong reference is a settings edit, not a
// rebuild -- this constant has flip-flopped once and can't be verified without mouse-look.
inline double g_compass_yaw_offset = 180.0;   // default 180: with CompassFlipEast on, a north
                                              // marker showed south (clean 180) -- see below
// Reflect the WORLD east axis before bearing markers (CompassFlipEast). A rotation
// (yaw offset) can't fix a left/right MIRROR -- turn-response stays correct (turn right
// -> markers slide left) yet a marker that is actually NE shows at NW (Kenny). That is
// the world bearing frame being reflected relative to the yaw frame; flipping the east
// delta corrects it while leaving the turn response intact. Default ON (Kenny's un-
// flipped build mirrored). With this + CompassYawOffset, any orientation is reachable
// from settings.txt -- no rebuild to chase the sign (mouse-look can't be scripted).
inline bool g_compass_flip_east = true;
// Fire the lucky-Pal range alert (LuckySound). Interim = a log line; a real in-engine cue
// comes in a follow-up (an OS beep is a USER32 import that breaks the DLL's import allowlist
// and is unreliable through Proton). Default on.
inline bool g_lucky_sound = true;
// How often each live actor layer (eggs / dungeons / lucky) refreshes on the compass WITHOUT
// opening the map, in seconds (CompassLiveSec). The three rotate one-per-fire, so a fire lands
// every g_compass_live_sec/3 s. Lower = snappier, higher = less CPU. Default 6.
inline double g_compass_live_sec = 6.0;

// Track-one-Pal (species layer). The INTERNAL Pal name (e.g. SheepBall = Lamball),
// as it appears in DT_PalWildSpawner's Pal_N columns. When set and the "Pal" layer
// is on, the map marks every placement that spawns this species (join over
// DT_PalSpawnerPlacement x DT_PalWildSpawner). Default is a common starter so the
// feature is visible on first toggle; change via TrackPal in settings (an in-game
// picker comes next). Empty = track nothing.
inline std::wstring g_track_pal = L"SheepBall";
// Effigies-by-type filter (#43). Each effigy is a relic with an EPalRelicType
// (0..12; Data::kEffigyType[] parallels kEffigies). -1 = show all types (default);
// 0..12 = show only that type, and the compass points to the nearest of it. Set
// in-game via the EFFIGIES type picker, persisted as TrackRelic.
inline int g_track_relic = -1;
static_assert(std::size(Data::kEffigyType) == std::size(Data::kEffigies),
              "kEffigyType must stay parallel to kEffigies -- regenerate cairn_relic_types.hpp");
// Placement: same two-letter anchor grid as the minimap, default TC (top-centre,
// under vanilla's own compass bar). Margins push in from an edge.
inline double g_compass_ax = 0.5;   // horizontal: 0 left, 0.5 centre, 1 right
inline double g_compass_ay = 0.0;   // vertical:   0 top,  0.5 centre, 1 bottom
inline double g_compass_ox = 0.0;
inline double g_compass_oy = 64.0;
// Config-popup launcher: a cog button on the open map (top-right), and the F5 hotkey.
// The button replaces the old "Configure map" text button. X/Y push in from the
// top-right corner so Kenny can nudge it to a spot that feels natural.
inline double g_menu_btn_size = 40.0;
inline double g_menu_btn_x = 0.0;
inline double g_menu_btn_y = 0.0;

    // On-screen marker sizes, before DotScale. Sized for icons, not for the plain
    // coloured dots these started as -- an item icon has to be recognised, not
    // merely seen, and vanilla's own map markers are ~40px for comparison.
    //
    // DotGrowth stays 0 on purpose: a marker's job is to pin an exact spot, and a
    // marker that grows with zoom smothers the very thing it points at by the time
    // you are zoomed in far enough to care. Constant on-screen size is the feature.
    // Users who want bigger set DotScale (1.33 ~ double the original 14px).
    inline constexpr double kLayerDotPx = 21.0;    // ore/resource/POI layers
    inline constexpr double kEggDotPx = 27.0;      // live eggs, deliberately louder
    inline constexpr double kCollectDotPx = 30.0;  // effigies + notes, rarest of all
    // Live POI layers, which draw vanilla's compass glyphs rather than item art.
    // Those glyphs are drawn for a compass strip and carry a lot of transparent
    // margin, so the SAME box renders a visibly smaller mark than an item icon
    // does -- the size below is compensating for the art, not shouting louder.
    inline constexpr double kPoiDotPx = 32.0;

    // ------------------------------------------------------------ engine facade
    // ⚠ SPEC 2.6: everything here runs on the game thread (on_update).
    namespace Engine
    {
        // BlueprintCallable param blocks (natural MSVC layout, x64).
        struct ParamsGetChildrenCount
        {
            int32_t ReturnValue{};
        };
        struct ParamsGetChildAt
        {
            int32_t Index{};
            UObject* ReturnValue{};
        };
        struct ParamsIsVisible
        {
            bool ReturnValue{};
        };
        struct ParamsAddChildToCanvas
        {
            UObject* Content{};
            UObject* ReturnValue{};
        };
        // UPanelWidget::RemoveChild(UWidget*) -> bool. Detaches a stale overlay we
        // left on a reused map body before building a fresh one (ensure_layer_canvas).
        struct ParamsRemoveChild
        {
            UObject* Content{};
            bool ReturnValue{};
        };
        struct FVector2D_
        {
            double X{}, Y{};
        };
        struct ParamsSetPosition
        {
            FVector2D_ InPosition{};
        };
        struct ParamsSetSize
        {
            FVector2D_ InSize{};
        };
        struct ParamsSetAlignment
        {
            FVector2D_ InAlignment{};
        };
        struct ParamsSetAutoSize
        {
            bool bAutoSize{};
        };
        // UWidget::SetRenderScale(FVector2D) -- scales about RenderTransformPivot,
        // which defaults to the widget's centre, so it resizes without moving the dot.
        struct ParamsSetRenderScale
        {
            FVector2D_ Scale{};
        };
        struct FLinearColor_
        {
            float R{}, G{}, B{}, A{};
        };
        struct ParamsSetColorAndOpacity
        {
            FLinearColor_ InColorAndOpacity{};
        };
        // UTextBlock::SetColorAndOpacity takes an FSlateColor (SlateCore.hpp: 0x14 =
        // FLinearColor SpecifiedColor @0x00 + ESlateColorStylingMode ColorUseRule @0x10),
        // NOT a bare FLinearColor. Passing only the colour left ColorUseRule reading stack
        // garbage, flipping text white (UseColor_Specified=0) <-> black (Foreground=2).
        struct ParamsSetTextColor
        {
            FLinearColor_ SpecifiedColor{};
            uint8_t ColorUseRule{};   // 0 = UseColor_Specified
        };
        struct ParamsSetVisibility
        {
            uint8_t InVisibility{};
        };
        struct ParamsSetZOrder
        {
            int32_t InZOrder{};
        };
        struct ParamsSetOffsets
        {
            float Left{}, Top{}, Right{}, Bottom{};   // FMargin
        };
        struct ParamsSetAnchors
        {
            double MinX{}, MinY{}, MaxX{}, MaxY{};   // FAnchors
        };
        struct ParamsSetBrushFromTexture
        {
            UObject* Texture{};
            bool bMatchSize{};
        };
        // UImage::SetBrushResourceObject(UObject*) -- unlike SetBrushFromTexture (which
        // is typed UTexture2D* and no-ops on a render target), this takes any texture-
        // like object, so it is how a UTextureRenderTarget2D gets onto an Image.
        struct ParamsSetBrushResourceObject
        {
            UObject* ResourceObject{};
        };
        // Terrain minimap: a raw render target via SetBrushResourceObject renders flat
        // yellow on this build (the RT-display is broken, not the capture). PalMiniMap
        // and Palworld's own pal-preview UI both wrap the RT in a MATERIAL. We reuse the
        // vanilla M_CapturedMaterial (UI material with a texture parameter literally named
        // "RenderTarget") the same way.
        // UImage::SetBrushFromMaterial(UMaterialInterface*) -- point the brush at a material.
        struct ParamsSetBrushFromMaterial
        {
            UObject* Material{};
        };
        // UImage::GetDynamicMaterial() -> UMaterialInstanceDynamic*. Mints a MID from the
        // brush's current material, swaps the brush to it, and returns it -- the MID is
        // what SetTextureParameterValue mutates.
        struct ParamsGetDynamicMaterial
        {
            UObject* ReturnValue{};
        };
        // UMaterialInstanceDynamic::SetTextureParameterValue(FName ParameterName, UTexture*).
        // FName is 8 bytes in the frame (cf. ParamsOpenLevel::LevelName @0x08); Value @0x08.
        // Offsets are checked with verify_params before the call.
        struct ParamsSetTextureParameterValue
        {
            FName ParameterName{FName()};   // 0x00
            UObject* Value{};               // 0x08
        };
        // USceneCaptureComponent::HideActorComponents(AActor*, bool) -- exclude an actor
        // from THIS capture only (main view untouched). Strips pals/players/NPCs from the
        // terrain capture so it reads as a clean map, not a top-down photo of the scene.
        struct ParamsHideActorComponents
        {
            UObject* InActor{};
            bool bIncludeFromChildActors{};
        };
        // USceneCaptureComponent::HideComponent(UPrimitiveComponent*) -- exclude one
        // component (a floating WidgetComponent / billboard base icon) from the capture.
        struct ParamsHideComponent
        {
            UObject* InComponent{};
        };
        struct ParamsSetIsChecked
        {
            bool InIsChecked{};
        };
        // A void, no-argument call. UFunction::ParmsSize is 0, so ProcessEvent
        // never reads this buffer; it exists only to satisfy call()'s template.
        struct ParamsNone
        {
        };

        struct ParamsIsUnlockMapPoint
        {
            bool ReturnValue{};
        };
        struct ParamsIsChecked
        {
            bool ReturnValue{};
        };
        // UPalIndividualCharacterParameter::IsRarePal (Pal.hpp:24738): no args, returns true
        // for the golden "lucky" wild-Pal variant.
        struct ParamsIsRarePal
        {
            bool ReturnValue{};
        };
        // UPalCoopSkillSearchSystem::IsRunning (Pal.hpp:19960)
        struct ParamsIsRunning
        {
            bool ReturnValue{};
        };
        struct ParamsSetText
        {
            FText InText{};
        };
        // UWidget::SetClipping (UMG.hpp:1884). EWidgetClipping::ClipToBounds = 1
        // (SlateCore_enums.hpp:410). A TextBlock does NOT clip by default: it happily
        // draws past its slot and over whatever is next to it.
        struct ParamsSetClipping
        {
            uint8_t InClipping{};
        };
        // UTextLayoutWidget::SetJustification. ETextJustify::{Left=0,Center=1,Right=2}
        // (Slate_enums.hpp:136). Right-justifying the distance column keeps the
        // numbers on a common edge, so they read as a column instead of ragged text.
        struct ParamsSetJustification
        {
            uint8_t InJustification{};
        };
        struct FVector_
        {
            double X{}, Y{}, Z{};
        };
        struct ParamsGetActorLocation
        {
            FVector_ ReturnValue{};
        };
        // UPalLocationPoint::GetLocation() -- the POI equivalent of the above.
        struct ParamsGetLocation
        {
            FVector_ ReturnValue{};
        };
        struct ParamsGetPawn
        {
            UObject* ReturnValue{};
        };
        // UWidget::GetOwningPlayer() -> APlayerController* (UMG.hpp:1916). Owning
        // controller straight off a HUD widget -- no object-array walk, unlike
        // FindFirstOf. This is how the minimap gets the player without scanning.
        struct ParamsGetOwningPlayer
        {
            UObject* ReturnValue{};
        };
        struct FRotator_
        {
            double Pitch{}, Yaw{}, Roll{};   // CoreUObject.hpp:633, 3 doubles (LWC)
        };
        // AActor::K2_GetActorRotation() -> FRotator (Engine.hpp:7989).
        struct ParamsGetActorRotation
        {
            FRotator_ ReturnValue{};
        };
        // UWidget::SetRenderTransformAngle(float) -- rotates about the render pivot,
        // clockwise degrees in screen space (UMG.hpp:1870).
        struct ParamsSetRenderTransformAngle
        {
            float Angle{};
        };
        // FTransform (CoreUObject, Size 0x60). FQuat @0x00, Translation @0x20, then
        // 8 bytes of pad (FTransform is 16-aligned), Scale3D @0x40, 8 bytes tail pad.
        // The pads are real -- 10 contiguous doubles would be 0x50 and mis-place
        // CollisionHandlingOverride on the spawn call. alignas(16) so the compiler
        // also 16-aligns it inside a param frame: FinishSpawningActor's SpawnTransform
        // sits at 0x10 (after the Actor pointer + 8 pad), not 0x08.
        struct alignas(16) FTransform_
        {
            double QX{}, QY{}, QZ{}, QW{1.0};   // 0x00 identity quat
            double TX{}, TY{}, TZ{};            // 0x20 translation
            double pad0_{};                     // 0x38
            double SX{1.0}, SY{1.0}, SZ{1.0};   // 0x40 scale
            double pad1_{};                     // 0x58
        };
        static_assert(sizeof(FTransform_) == 0x60);
        static_assert(offsetof(FTransform_, TX) == 0x20);
        static_assert(offsetof(FTransform_, SX) == 0x40);
        // UKismetRenderingLibrary::CreateRenderTarget2D (Engine.hpp:14707).
        struct ParamsCreateRenderTarget2D
        {
            UObject* WorldContextObject{};   // 0x00
            int32_t Width{};                 // 0x08
            int32_t Height{};                // 0x0C
            uint8_t Format{};                // 0x10  ETextureRenderTargetFormat (RTF_RGBA8 = 2)
            FLinearColor_ ClearColor{};      // 0x14
            bool bAutoGenerateMipMaps{};     // 0x24
            UObject* ReturnValue{};          // 0x28
        };
        // UGameplayStatics::BeginDeferredActorSpawnFromClass (Engine.hpp:13443).
        struct ParamsBeginDeferredActorSpawn
        {
            UObject* WorldContextObject{};        // 0x00
            UObject* ActorClass{};                // 0x08 TSubclassOf
            FTransform_ SpawnTransform{};         // 0x10
            uint8_t CollisionHandlingOverride{};  // 0x70 (AlwaysSpawn = 1)
            UObject* Owner{};                     // 0x78
            UObject* ReturnValue{};               // 0x80
        };
        // UGameplayStatics::FinishSpawningActor (Engine.hpp:13421). SpawnTransform is
        // FTransform (16-aligned), so it sits at 0x10 after Actor + 8 bytes of pad.
        struct ParamsFinishSpawningActor
        {
            UObject* Actor{};              // 0x00
            FTransform_ SpawnTransform{};  // 0x10 (alignas(16) pads 0x08..0x10)
            UObject* ReturnValue{};        // 0x70
        };
        // AActor::K2_TeleportTo (Engine.hpp:7976) -- no sweep/hit-result out param.
        struct ParamsTeleportTo
        {
            FVector_ DestLocation{};    // 0x00
            FRotator_ DestRotation{};   // 0x18
            bool ReturnValue{};         // 0x30
        };
        // UWidget::GetParent() -> UPanelWidget* (UMG.hpp:1914). Same shape as the
        // pawn getter: one pointer return, no arguments.
        struct ParamsGetParent
        {
            UObject* ReturnValue{};
        };
        // UCanvasPanelSlot::LayoutData (FAnchorData @0x38, UMG.hpp). Read directly
        // rather than through GetPosition(), because a property read cannot be
        // rejected the way a call can, and this runs on a widget we do not own.
        //
        // FMargin is FLOAT (0x10 = 4 floats) while FAnchors/FVector2D are DOUBLE
        // (0x20 = 4 doubles, 0x10 = 2 doubles) -- UE5 LWC widened the vectors but not
        // the margin. Mixing those up silently shifts every field after it.
        struct FMargin_
        {
            float Left{}, Top{}, Right{}, Bottom{};
        };
        struct FAnchors_
        {
            double MinX{}, MinY{}, MaxX{}, MaxY{};
        };
        struct FAnchorData_
        {
            FMargin_ Offsets{};
            FAnchors_ Anchors{};
            double AlignX{}, AlignY{};
        };
        // UWidget::RenderTransform (FWidgetTransform @0x98, UMG.hpp:1848). The canvas
        // slot puts every compass icon at the same pos(0,0) anchor(0.5,0), so THIS is
        // what actually moves an icon along the compass -- the slot never does.
        struct FWidgetTransform_
        {
            double TransX{}, TransY{};
            double ScaleX{}, ScaleY{};
            double ShearX{}, ShearY{};
            float Angle{};
        };
        static_assert(sizeof(FMargin_) == 0x10);
        static_assert(sizeof(FAnchors_) == 0x20);
        static_assert(sizeof(FAnchorData_) == 0x40);
        // The TArray header, to read RegisteredLocationIds.Num() without marshalling
        // the elements. Reading Num across the call is what makes the experiment
        // self-reporting: it says whether the register ACCEPTED our parameter,
        // separately from whether anything drew.
        struct FArrayHeader_
        {
            void* Data{};
            int32_t Num{};
            int32_t Max{};
        };

        struct FGuid_
        {
            uint32_t A{}, B{}, C{}, D{};
        };
        // APalPlayerController::GetPlayerUId (Pal.hpp:13046)
        struct ParamsGetPlayerUId
        {
            FGuid_ ReturnValue{};
        };
        // UPalLocationManager::GetLocationPoint(const FGuid& ID) -> UPalLocationPoint*
        // Offsets verified at runtime before the call, as always.
        struct ParamsGetLocationPoint
        {
            FGuid_ ID{};
            UObject* ReturnValue{};
        };
        // UPalCoopSkillSearchBase::Start(const FVector& Origin, int32 Rank,
        // const FGuid& RequestPlayerUId). FGuid is four uint32 -> 4-byte aligned, so it
        // packs at 0x1C right behind the int32, NOT at 0x20. That exact assumption cost
        // a silent frame corruption on SearchMapObjects; offsets are checked at runtime
        // before the call regardless.
        struct ParamsStart
        {
            FVector_ Origin{};
            int32_t Rank{};
            FGuid_ RequestPlayerUId{};
        };
        // FPalCoopSkillSearchResultParameter (Pal.hpp:1967, size 0x78). Laid out by
        // hand because ProcessEvent copies this struct into the param frame verbatim
        // -- every offset below is from the dump, and the padding is load-bearing.
        struct FPalCoopSkillSearchResultParameter_
        {
            uint8_t SearchType{};           // 0x00  EPalCoopSkillSearchType
            uint8_t pad0_[7]{};             // 0x01
            FVector_ Location{};            // 0x08  (0x18)
            FGuid_ InstanceId{};            // 0x20
            uint8_t RelicType{};            // 0x30  EPalRelicType
            uint8_t pad1_[7]{};             // 0x31
            uint8_t IndividualId[0x30]{};   // 0x38  FPalInstanceID (zeroed = empty)
            FGuid_ RequestPlayerUId{};      // 0x68
        };
        static_assert(sizeof(FPalCoopSkillSearchResultParameter_) == 0x78,
                      "FPalCoopSkillSearchResultParameter must match the dump exactly (Pal.hpp:1976)");
        static_assert(offsetof(FPalCoopSkillSearchResultParameter_, Location) == 0x08);
        static_assert(offsetof(FPalCoopSkillSearchResultParameter_, InstanceId) == 0x20);
        static_assert(offsetof(FPalCoopSkillSearchResultParameter_, IndividualId) == 0x38);
        static_assert(offsetof(FPalCoopSkillSearchResultParameter_, RequestPlayerUId) == 0x68);
        struct ParamsRegisterToCompass
        {
            FPalCoopSkillSearchResultParameter_ ResultParameter{};
        };
        // UPalCoopSkillSearchSystem::CreateSearchObject (Pal.hpp:19962).
        // TSubclassOf<T> is a UClass* in the param frame.
        struct ParamsCreateSearchObject
        {
            UObject* SearchClass{};
            UObject* ReturnValue{};
        };
        // UPalGameInstance::SelectWorldSaveDirectoryName (Pal.hpp:22352) -- the same
        // call the title menu makes when you click a world.
        struct ParamsSelectWorldSaveDirectoryName
        {
            FString WorldSaveDirectoryName{};
            bool ReturnValue{};
        };
        // UGameplayStatics::OpenLevel (Engine.hpp:13364). Offsets are ASSUMED and
        // checked at runtime before calling -- same rule as SearchMapObjects, where
        // an assumed offset was wrong by 4 bytes.
        struct ParamsOpenLevel
        {
            UObject* WorldContextObject{};   // 0x00
            FName LevelName{FName()};        // 0x08
            bool bAbsolute{};                // 0x10
            uint8_t pad0_[7]{};              // 0x11
            FString Options{};               // 0x18
        };
        // UPalCoopSkillSearchMapObject::SearchMapObjects (Pal.hpp:19949):
        //   (const TArray<FName>&, const FVector&, float, const FGuid&)
        // Reference params are still passed BY VALUE in the ProcessEvent frame, so
        // this is the four values laid end to end. The offsets are ASSUMED from UE's
        // alignment rules and CHECKED against the UFunction's own properties before
        // the call -- see search_params_ok. A wrong frame here would corrupt
        // memory rather than fail, so it is not something to find out by trying.
        struct ParamsSearchMapObjects
        {
            FArrayHeader_ SearchMapObjIds{};   // 0x00  TArray<FName>
            FVector_ Origin{};                 // 0x10
            float SearchRadius{};              // 0x28
            FGuid_ RequestPlayerUId{};         // 0x2C -- measured, NOT 0x30
        };
        // 0x2C, not the 0x30 I assumed. FGuid is four uint32 so it aligns to 4 and
        // packs straight after the float with no padding; I had assumed 8-byte
        // alignment. The runtime check caught it and skipped the call -- had it been
        // trusted, ProcessEvent would have written the GUID four bytes past where the
        // engine reads it, corrupting the frame instead of failing. Offsets below are
        // the ones the UFunction itself reported.
        static_assert(offsetof(ParamsSearchMapObjects, Origin) == 0x10);
        static_assert(offsetof(ParamsSearchMapObjects, SearchRadius) == 0x28);
        static_assert(offsetof(ParamsSearchMapObjects, RequestPlayerUId) == 0x2C);

        enum : uint8_t
        {
            Vis_Visible = 0,
            Vis_Collapsed = 1,
            Vis_Hidden = 2,
            Vis_HitTestInvisible = 3,
            Vis_SelfHitTestInvisible = 4,
        };

        template <typename Params>
        auto call(UObject* obj, const wchar_t* fn_name, Params& params) -> bool
        {
            if (!obj)
            {
                return false;
            }
            UFunction* fn = obj->GetFunctionByNameInChain(FName(fn_name, FNAME_Find));
            if (!fn)
            {
                return false;
            }
            obj->ProcessEvent(fn, &params);
            return true;
        }

        inline auto children_count(UObject* panel) -> int32_t
        {
            ParamsGetChildrenCount p{};
            return call(panel, L"GetChildrenCount", p) ? p.ReturnValue : 0;
        }

        inline auto child_at(UObject* panel, int32_t i) -> UObject*
        {
            ParamsGetChildAt p{i, nullptr};
            return call(panel, L"GetChildAt", p) ? p.ReturnValue : nullptr;
        }

        inline auto widget_name(UObject* w) -> std::wstring
        {
            return w ? w->GetName() : std::wstring{};
        }

        // World-space actor location via K2_GetActorLocation (FVector return).
        inline auto actor_location(UObject* actor, double& x, double& y, double& z) -> bool
        {
            ParamsGetActorLocation p{};
            if (!call(actor, L"K2_GetActorLocation", p))
            {
                return false;
            }
            x = p.ReturnValue.X;
            y = p.ReturnValue.Y;
            z = p.ReturnValue.Z;
            return true;
        }

        inline auto class_name(UObject* w) -> std::wstring
        {
            return (w && w->GetClassPrivate()) ? w->GetClassPrivate()->GetName() : std::wstring{};
        }

        inline auto find_descendant(UObject* w, const std::wstring& name, int depth = 0) -> UObject*
        {
            if (!w || depth > 8)
            {
                return nullptr;
            }
            if (widget_name(w) == name)
            {
                return w;
            }
            const int32_t n = children_count(w);
            for (int32_t i = 0; i < n; ++i)
            {
                if (auto* r = find_descendant(child_at(w, i), name, depth + 1))
                {
                    return r;
                }
            }
            return nullptr;
        }

        // Count descendants whose name starts with `prefix` (StaticConstructObject may
        // suffix "_1" when a swept name is still held pending GC, so prefix not exact).
        inline auto count_descendants_prefix(UObject* w, const std::wstring& prefix, int depth = 0) -> int
        {
            if (!w || depth > 24)   // map body -> WidgetTree -> ... -> our canvas is deep
            {
                return 0;
            }
            int c = widget_name(w).starts_with(prefix) ? 1 : 0;
            const int32_t n = children_count(w);
            for (int32_t i = 0; i < n; ++i)
            {
                c += count_descendants_prefix(child_at(w, i), prefix, depth + 1);
            }
            return c;
        }

        // Canvas slot offsets: LayoutData.Offsets (FMargin, 4 floats) leads the struct.
        inline auto slot_position(UObject* widget, double& x, double& y) -> bool
        {
            if (!widget)
            {
                return false;
            }
            auto** slot = widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot"));
            if (!slot || !*slot)
            {
                return false;
            }
            auto* layout = (*slot)->GetValuePtrByPropertyNameInChain<float>(STR("LayoutData"));
            if (!layout)
            {
                return false;
            }
            x = layout[0];   // Offsets.Left
            y = layout[1];   // Offsets.Top
            return true;
        }
    } // namespace Engine


    // ----------------------------------------------------------- dot styling
    // Round dots without any texture: FSlateBrush.DrawAs = RoundedBox with a
    // fixed corner radius, written via reflection-resolved offsets.
    namespace Style
    {
        struct Offsets
        {
            int32_t draw_as = -1, outline = -1, radii = -1, rounding = -1, tint = -1;
            int32_t image_size = -1;
            // Within the OutlineSettings sub-struct: Color (FSlateColor) and Width, for
            // drawing a ring border on a RoundedBox. Optional -- resolve() does not gate
            // `resolved` on them, so a build missing them still gets fills.
            int32_t ocolor = -1, owidth = -1;
            bool resolved = false;
        };

        // The panel's one accent. Every checkbox and every row label uses it:
        // the swatch is not a colour legend, so it does not carry the layer's
        // colour -- the label already names the layer, and per-layer colour on
        // a near-black panel (0.02,0.02,0.04) is unreadable at the dark end
        // (Coal 0x202020, Oil 0x140A0A). Layer colour lives on the map dots.
        // Same gold as the Eggs row and the live egg dots.
        inline constexpr float kAccentR = 1.0f;
        inline constexpr float kAccentG = 0.82f;
        inline constexpr float kAccentB = 0.15f;

        // Panel legend swatch, left of each checkbox. Slightly larger than the
        // 14px box so an icon is actually readable at a glance.
        inline constexpr double kSwatchPx = 16.0;

        // Two icon families, told apart by their name prefix:
        //
        //   T_itemicon_*  full-colour item art, /Game/Others/InventoryItemIcon/Texture/
        //   T_icon_*      monochrome UI glyph,  /Game/Pal/Texture/UI/IngameMenu/
        //
        // Item art is drawn untinted -- coal is meant to look like coal. A glyph
        // carries no colour of its own, so it takes the layer's, which also keeps
        // it consistent with that layer's legend.
        inline auto icon_is_glyph(const wchar_t* icon) -> bool
        {
            return icon && std::wstring_view(icon).starts_with(L"T_icon_");
        }

        // Colours in cairn_data.hpp are authored as 8-bit sRGB (what you'd type in
        // a paint program). UMG's SetColorAndOpacity treats its FLinearColor as
        // *linear* and re-applies the sRGB gamma curve on display, so a byte handed
        // straight through renders washed out -- measured: Outpost (255,60,60) came
        // out (254,135,135). Undo the curve once here so the round trip lands on the
        // authored colour. 255 maps to 1.0, so untinted white item art is unchanged.
        // Alpha is linear; do NOT pass it through this.
        inline auto srgb8(uint8_t c) -> float
        {
            const float s = c / 255.0f;
            return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
        }

        inline auto icon_dir(const wchar_t* icon) -> const wchar_t*
        {
            if (!icon)
            {
                return nullptr;
            }
            const std::wstring_view n{icon};
            if (n.starts_with(L"T_itemicon_"))
            {
                return L"/Game/Others/InventoryItemIcon/Texture/";
            }
            // Must precede the T_icon_ rule below: the compass/map family shares that
            // prefix but lives in UI/InGame, not UI/IngameMenu. Ordered the other way
            // round these resolve to a package that does not exist and the load fails
            // silently -- a dot with no icon, which looks like every other icon bug.
            if (n.starts_with(L"T_icon_compass_") || n.starts_with(L"T_icon_map_"))
            {
                return L"/Game/Pal/Texture/UI/InGame/";
            }
            if (n.starts_with(L"T_icon_"))
            {
                return L"/Game/Pal/Texture/UI/IngameMenu/";
            }
            return nullptr;
        }

        inline auto resolve(Offsets& off) -> bool
        {
            if (off.resolved)
            {
                return true;
            }
            auto* brush_struct =
                UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/SlateCore.SlateBrush"));
            auto* outline_struct = UObjectGlobals::StaticFindObject<UStruct*>(
                nullptr, nullptr, STR("/Script/SlateCore.SlateBrushOutlineSettings"));
            if (!brush_struct || !outline_struct)
            {
                return false;
            }
            for (FProperty* prop : brush_struct->ForEachProperty())
            {
                if (prop->GetName() == STR("DrawAs"))
                {
                    off.draw_as = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("OutlineSettings"))
                {
                    off.outline = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("TintColor"))
                {
                    off.tint = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("ImageSize"))
                {
                    off.image_size = prop->GetOffset_Internal();
                }
            }
            for (FProperty* prop : outline_struct->ForEachProperty())
            {
                if (prop->GetName() == STR("CornerRadii"))
                {
                    off.radii = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("RoundingType"))
                {
                    off.rounding = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("Color"))
                {
                    off.ocolor = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("Width"))
                {
                    off.owidth = prop->GetOffset_Internal();
                }
            }
            off.resolved = off.draw_as >= 0 && off.outline >= 0 && off.radii >= 0 && off.rounding >= 0;
            return off.resolved;
        }

        // paint a raw FSlateBrush block as a tinted rounded box, optionally with a ring
        // border (border_w > 0) in (br,bg,bb,ba) -- for the active-pill highlight.
        inline auto paint_brush(uint8_t* brush, const Offsets& off, float r, float g, float b, float a,
                                double radius, double border_w = 0.0, float br = 0.0f, float bg = 0.0f,
                                float bb = 0.0f, float ba = 1.0f) -> void
        {
            brush[off.draw_as] = 4;   // RoundedBox
            uint8_t* outline = brush + off.outline;
            outline[off.rounding] = 0;   // FixedRadius
            auto* radii = reinterpret_cast<double*>(outline + off.radii);
            radii[0] = radii[1] = radii[2] = radii[3] = radius;
            if (off.owidth >= 0)
            {
                *reinterpret_cast<float*>(outline + off.owidth) = static_cast<float>(border_w);
            }
            if (border_w > 0.0 && off.ocolor >= 0)
            {
                auto* oc = reinterpret_cast<float*>(outline + off.ocolor);
                oc[0] = br;
                oc[1] = bg;
                oc[2] = bb;
                oc[3] = ba;
                *(outline + off.ocolor + 0x10) = 0;   // FSlateColor ColorUseRule = UseColor_Specified
            }
            if (off.tint >= 0)
            {
                // FSlateColor: FLinearColor SpecifiedColor leads the struct
                auto* col = reinterpret_cast<float*>(brush + off.tint);
                col[0] = r;
                col[1] = g;
                col[2] = b;
                col[3] = a;
            }
        }

        // Style a runtime CheckBox so it is visible AND clickable in a shipped
        // game (default brushes are stripped -> 0px invisible). Unchecked =
        // dark box, checked = accent gold; hovered/pressed mirror them. Gold
        // against the dark box is a luminance gap of 0.81 vs 0.12, so the
        // on/off state is unmistakable on every row.
        inline auto make_checkbox(UObject* cb) -> void
        {
            static Offsets off;
            if (!resolve(off) || off.tint < 0)
            {
                return;
            }
            static int32_t style_off = -1;
            static int32_t f_unchecked = -1, f_unhover = -1, f_unpress = -1;
            static int32_t f_checked = -1, f_chkhover = -1, f_chkpress = -1;
            if (style_off < 0)
            {
                if (auto* cb_class = cb->GetClassPrivate())
                {
                    for (FProperty* p : cb_class->ForEachPropertyInChain())
                    {
                        if (p->GetName() == STR("WidgetStyle"))
                        {
                            style_off = p->GetOffset_Internal();
                        }
                    }
                }
                auto* cbs = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr,
                                                                       STR("/Script/SlateCore.CheckBoxStyle"));
                if (cbs)
                {
                    for (FProperty* p : cbs->ForEachProperty())
                    {
                        const auto n = p->GetName();
                        if (n == STR("UncheckedImage")) f_unchecked = p->GetOffset_Internal();
                        else if (n == STR("UncheckedHoveredImage")) f_unhover = p->GetOffset_Internal();
                        else if (n == STR("UncheckedPressedImage")) f_unpress = p->GetOffset_Internal();
                        else if (n == STR("CheckedImage")) f_checked = p->GetOffset_Internal();
                        else if (n == STR("CheckedHoveredImage")) f_chkhover = p->GetOffset_Internal();
                        else if (n == STR("CheckedPressedImage")) f_chkpress = p->GetOffset_Internal();
                    }
                }
            }
            if (style_off < 0 || f_unchecked < 0 || f_checked < 0)
            {
                return;
            }
            auto* style = cb->GetValuePtrByPropertyNameInChain<uint8_t>(STR("WidgetStyle"));
            if (!style)
            {
                return;
            }
            auto dark = [&](int32_t f) {
                if (f >= 0) paint_brush(style + f, off, 0.12f, 0.12f, 0.14f, 1.0f, 3.0);
            };
            auto accent = [&](int32_t f) {
                if (f >= 0) paint_brush(style + f, off, kAccentR, kAccentG, kAccentB, 1.0f, 3.0);
            };
            dark(f_unchecked);
            dark(f_unhover);
            dark(f_unpress);
            accent(f_checked);
            accent(f_chkhover);
            accent(f_chkpress);
        }

        // Resolve the CheckBoxStyle brush-field offsets once (shared by make_checkbox /
        // make_pill). Returns false if the reflection is unavailable.
        inline auto checkbox_style_offsets(UObject* cb, int32_t& style_off, int32_t f[6]) -> bool
        {
            static int32_t s_style = -1, s_f[6] = {-1, -1, -1, -1, -1, -1};
            if (s_style < 0)
            {
                if (auto* cb_class = cb->GetClassPrivate())
                {
                    for (FProperty* p : cb_class->ForEachPropertyInChain())
                    {
                        if (p->GetName() == STR("WidgetStyle"))
                        {
                            s_style = p->GetOffset_Internal();
                        }
                    }
                }
                auto* cbs = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr,
                                                                       STR("/Script/SlateCore.CheckBoxStyle"));
                if (cbs)
                {
                    for (FProperty* p : cbs->ForEachProperty())
                    {
                        const auto n = p->GetName();
                        if (n == STR("UncheckedImage")) s_f[0] = p->GetOffset_Internal();
                        else if (n == STR("UncheckedHoveredImage")) s_f[1] = p->GetOffset_Internal();
                        else if (n == STR("UncheckedPressedImage")) s_f[2] = p->GetOffset_Internal();
                        else if (n == STR("CheckedImage")) s_f[3] = p->GetOffset_Internal();
                        else if (n == STR("CheckedHoveredImage")) s_f[4] = p->GetOffset_Internal();
                        else if (n == STR("CheckedPressedImage")) s_f[5] = p->GetOffset_Internal();
                    }
                }
            }
            style_off = s_style;
            for (int i = 0; i < 6; ++i) f[i] = s_f[i];
            return s_style >= 0 && s_f[0] >= 0 && s_f[3] >= 0;
        }

        // Invisible, full-size clickable CheckBox: every brush transparent so nothing
        // shows, but the widget keeps its slot-sized HIT AREA. The CheckBox's own box
        // brush does NOT stretch to fill the cell (it draws a small box), so the pill card
        // + its ring are a separate Image behind (make_pill_bg); this catches the click
        // over the whole cell and drives the toggle poll.
        inline auto make_checkbox_clear(UObject* cb) -> void
        {
            static Offsets off;
            if (!resolve(off))
            {
                return;
            }
            int32_t style_off = -1, f[6];
            if (!checkbox_style_offsets(cb, style_off, f))
            {
                return;
            }
            auto* style = cb->GetValuePtrByPropertyNameInChain<uint8_t>(STR("WidgetStyle"));
            if (!style)
            {
                return;
            }
            for (int i = 0; i < 6; ++i)
            {
                if (f[i] >= 0) paint_brush(style + f[i], off, 0.0f, 0.0f, 0.0f, 0.0f, 6.0);
            }
        }

        // Draw a toggle PILL card on an Image that fills the whole cell: dim flat card
        // when off, brighter card with a bright TEAL ring when on (palworld.gg's selected
        // look). paint_brush sets the brush fill + outline directly, so leave the widget
        // tint white and the ring renders at full teal (not multiplied down).
        inline auto make_pill_bg(UObject* image_widget, bool active) -> void
        {
            static Offsets off;
            if (!resolve(off))
            {
                return;
            }
            auto* brush = image_widget->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Brush"));
            if (!brush)
            {
                return;
            }
            if (active)
            {
                paint_brush(brush, off, 0.11f, 0.17f, 0.25f, 1.0f, 7.0, 2.5, 0.20f, 0.90f, 0.85f, 1.0f);
            }
            else
            {
                paint_brush(brush, off, 0.055f, 0.085f, 0.13f, 0.85f, 7.0);
            }
        }

        // shrink a TextBlock's font (FSlateFontInfo.Size) via reflection
        inline auto set_font_size(UObject* text_widget, int32_t size) -> void
        {
            static int32_t size_off = -1;
            if (size_off < 0)
            {
                auto* fi = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr,
                                                                     STR("/Script/SlateCore.SlateFontInfo"));
                if (fi)
                {
                    for (FProperty* p : fi->ForEachProperty())
                    {
                        if (p->GetName() == STR("Size"))
                        {
                            size_off = p->GetOffset_Internal();
                        }
                    }
                }
            }
            if (size_off < 0)
            {
                return;
            }
            auto* font = text_widget->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Font"));
            if (font)
            {
                *reinterpret_cast<int32_t*>(font + size_off) = size;
            }
        }

        // Switch a dot's brush from RoundedBox to the icon texture.
        //
        // ESlateBrushDrawType (verified against this build's own reflection dump,
        // ue4ss/CXXHeaderDump/SlateCore_enums.hpp):
        //     NoDrawType=0, Box=1, Border=2, Image=3, RoundedBox=4
        // Image is 3. Upstream wrote 0 here and commented it as Image -- 0 is
        // NoDrawType, whose entire job is to draw nothing, so every icon was
        // explicitly told not to render. Texture, ImageSize and tint were all
        // irrelevant: Slate short-circuits before reaching them.
        //
        // ImageSize is set explicitly because SetBrushFromTexture is called with
        // bMatchSize=false and so never writes it. A RoundedBox brush ignores
        // ImageSize; Image does not.
        inline auto make_image(UObject* image_widget, double size) -> void
        {
            static Offsets off;
            if (!resolve(off))
            {
                return;
            }
            auto* brush = image_widget->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Brush"));
            if (!brush)
            {
                return;
            }
            if (off.image_size >= 0)
            {
                // FVector2D, 2 doubles under UE5 LWC (SlateCore.hpp: ImageSize @0x18)
                auto* dims = reinterpret_cast<double*>(brush + off.image_size);
                dims[0] = size;
                dims[1] = size;
            }
            brush[off.draw_as] = 3;   // ESlateBrushDrawType::Image
        }

        // radius defaults to 6 px (a lightly-rounded square, fine for tiny dots +
        // swatches). Pass half the widget's size for a TRUE circle at any size --
        // Slate clamps the corner radius to half the box, so a big radius = circle.
        //
        // border_w > 0 draws a ring outline in (br,bg,bb,ba) -- a map-pin border around
        // the fill. The outline Color is an FSlateColor (FLinearColor + ColorUseRule);
        // rule 0 = UseColor_Specified so the ring uses the given colour.
        inline auto make_round(UObject* image_widget, double radius = 6.0, double border_w = 0.0,
                               float br = 0.0f, float bg = 0.0f, float bb = 0.0f, float ba = 1.0f) -> void
        {
            static Offsets off;
            if (!resolve(off))
            {
                return;
            }
            auto* brush = image_widget->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Brush"));
            if (!brush)
            {
                return;
            }
            brush[off.draw_as] = 4;   // ESlateBrushDrawType::RoundedBox
            uint8_t* outline = brush + off.outline;
            outline[off.rounding] = 0;   // ESlateBrushRoundingType::FixedRadius
            auto* radii = reinterpret_cast<double*>(outline + off.radii);
            radii[0] = radii[1] = radii[2] = radii[3] = radius;
            if (off.owidth >= 0)
            {
                *reinterpret_cast<float*>(outline + off.owidth) = static_cast<float>(border_w);
            }
            if (border_w > 0.0 && off.ocolor >= 0)
            {
                // FSlateColor: FLinearColor SpecifiedColor @0x00 + ColorUseRule @0x10.
                auto* col = reinterpret_cast<float*>(outline + off.ocolor);
                col[0] = br;
                col[1] = bg;
                col[2] = bb;
                col[3] = ba;
                *(outline + off.ocolor + 0x10) = 0;   // UseColor_Specified
            }
        }
    } // namespace Style

    // -------------------------------------------------- collected state (P1.5)
    // Truth source: PalPlayerRecordData's <X>ObtainForInstanceFlag arrays;
    // keys are the actors' LevelObjectInstanceId as 32-hex FName strings
    // (grounded live 2026-07-13, probe run).
    namespace Collected
    {
        struct Offsets
        {
            int32_t items = -1, key = -1, value = -1, item_size = 0;
            bool resolved = false;
        };

        inline auto resolve(Offsets& off) -> bool
        {
            if (off.resolved)
            {
                return true;
            }
            auto* wrapper_struct = UObjectGlobals::StaticFindObject<UStruct*>(
                nullptr, nullptr, STR("/Script/Pal.PalPlayerRecordDataRepInfoArrayThreadSafe_BoolVal"));
            auto* item_struct = UObjectGlobals::StaticFindObject<UStruct*>(
                nullptr, nullptr, STR("/Script/Pal.PalPlayerRecordDataRepInfoThreadSafe_BoolVal"));
            if (!wrapper_struct || !item_struct)
            {
                return false;
            }
            for (FProperty* prop : wrapper_struct->ForEachProperty())
            {
                if (prop->GetName() == STR("Items"))
                {
                    off.items = prop->GetOffset_Internal();
                }
            }
            for (FProperty* prop : item_struct->ForEachProperty())
            {
                if (prop->GetName() == STR("Key"))
                {
                    off.key = prop->GetOffset_Internal();
                }
                if (prop->GetName() == STR("Value"))
                {
                    off.value = prop->GetOffset_Internal();
                }
            }
            off.item_size = item_struct->GetStructureSize();
            off.resolved = off.items >= 0 && off.key >= 0 && off.value >= 0 && off.item_size > 0;
            return off.resolved;
        }

        // union of all true-flag keys across the relic/note arrays
        inline auto gather(std::unordered_set<std::wstring>& out) -> bool
        {
            static Offsets off;
            if (!resolve(off))
            {
                return false;
            }
            auto* util_cdo =
                UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            auto* world_ctx = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (!util_cdo || !world_ctx)
            {
                return false;
            }
            struct
            {
                UObject* WorldContextObject{};
                UObject* ReturnValue{};
            } rec{world_ctx, nullptr};
            if (!Engine::call(util_cdo, L"GetLocalRecordData", rec) || !rec.ReturnValue)
            {
                return false;
            }
            static const wchar_t* kFlagArrays[] = {
                STR("RelicObtainForInstanceFlag_CapturePower"),
                STR("RelicObtainForInstanceFlag_HungerReduction"),
                STR("RelicObtainForInstanceFlag_SwimSpeed"),
                STR("RelicObtainForInstanceFlag_FoodDecayReduction"),
                STR("RelicObtainForInstanceFlag_JumpPower"),
                STR("RelicObtainForInstanceFlag_GliderSpeed"),
                STR("RelicObtainForInstanceFlag_ClimbSpeed"),
                STR("RelicObtainForInstanceFlag_StatusAilmentResist"),
                STR("RelicObtainForInstanceFlag_StaminaReduction"),
                STR("RelicObtainForInstanceFlag_SphereHoming"),
                STR("RelicObtainForInstanceFlag_ExpBonus"),
                STR("RelicObtainForInstanceFlag_RainbowPassiveRate"),
                STR("RelicObtainForInstanceFlag_MoveSpeed"),
                STR("NoteObtainForInstanceFlag"),
            };
            out.clear();
            for (const auto* arr_name : kFlagArrays)
            {
                auto* wrapper = rec.ReturnValue->GetValuePtrByPropertyNameInChain<uint8_t>(arr_name);
                if (!wrapper)
                {
                    continue;
                }
                struct RawArray
                {
                    uint8_t* data;
                    int32_t num;
                    int32_t max;
                };
                const auto* arr = reinterpret_cast<const RawArray*>(wrapper + off.items);
                for (int32_t i = 0; i < arr->num; ++i)
                {
                    const uint8_t* item = arr->data + static_cast<size_t>(i) * off.item_size;
                    if (*(item + off.value) == 0)
                    {
                        continue;
                    }
                    const auto* key = reinterpret_cast<const FName*>(item + off.key);
                    out.insert(key->ToString());
                }
            }
            return true;
        }

        inline auto guid_key(const uint32_t g[4]) -> std::wstring
        {
            wchar_t buf[36];
            swprintf(buf, 36, L"%08X%08X%08X%08X", g[0], g[1], g[2], g[3]);
            return buf;
        }

        // The LOCAL player's UNLOCKED fast-travel points: FastTravelPointUnlockFlag
        // (Pal.hpp:32008, same FPalPlayerRecordDataRepInfoArrayThreadSafe_BoolVal family as the
        // relic/note flags), keyed by the point's FastTravelPointID FName. This is PER-PLAYER
        // and replicated to the client -- unlike UPalLocationPointFastTravel::IsUnlockMapPoint,
        // whose bUnlockMapPoint is a world/host flag that on a dedicated server reports only the
        // ~22 auto-opened points, so towers the player personally unlocked still read "locked".
        // Same record + walk as gather(), one array.
        inline auto gather_ft_unlocked(std::unordered_set<std::wstring>& out) -> bool
        {
            static Offsets off;
            if (!resolve(off))
            {
                return false;
            }
            auto* util_cdo =
                UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            auto* world_ctx = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (!util_cdo || !world_ctx)
            {
                return false;
            }
            struct
            {
                UObject* WorldContextObject{};
                UObject* ReturnValue{};
            } rec{world_ctx, nullptr};
            if (!Engine::call(util_cdo, L"GetLocalRecordData", rec) || !rec.ReturnValue)
            {
                return false;
            }
            auto* wrapper =
                rec.ReturnValue->GetValuePtrByPropertyNameInChain<uint8_t>(STR("FastTravelPointUnlockFlag"));
            if (!wrapper)
            {
                return false;
            }
            struct RawArray
            {
                uint8_t* data;
                int32_t num;
                int32_t max;
            };
            const auto* arr = reinterpret_cast<const RawArray*>(wrapper + off.items);
            out.clear();
            for (int32_t i = 0; i < arr->num; ++i)
            {
                const uint8_t* item = arr->data + static_cast<size_t>(i) * off.item_size;
                if (*(item + off.value) == 0)
                {
                    continue;
                }
                const auto* key = reinterpret_cast<const FName*>(item + off.key);
                out.insert(key->ToString());
            }
            return true;
        }
    } // namespace Collected

    // ---------------------------------------------------------------- the mod
    class Mod final : public RC::CppUserModBase
    {
        bool m_unreal_ready = false;
        std::chrono::steady_clock::time_point m_last_tick{};

        // per-map-body state (SPEC 2.4): pool keyed by the live canvas
        std::wstring m_canvas_full_name;
        UObject* m_layer_canvas = nullptr;                 // our own CanvasPanel
        UObject* m_layer_slot = nullptr;                   // its canvas slot
        uint8_t m_mask_geom[64] = {};                      // last-seen mask LayoutData
        // layer ids: 0..N-1 = Data::kLayers index; 1000 = effigies, 1001 = notes,
        // 1002 = live eggs (no static positions; enumerated from loaded actors)
        static constexpr int kEffigyLayer = 1000;
        static constexpr int kNoteLayer = 1001;
        static constexpr int kEggLayer = 1002;
        // These three sit outside kLayers, so save_settings' loop over kLayers never
        // saw them and their toggles silently reset every launch. They need their own
        // settings keys. Keys are stable ids -- do not rename (a rename reads as an
        // unknown key and the toggle reverts to its default).
        struct ExtraLayer
        {
            int id;
            const wchar_t* key;
        };
        // Live POI layers: read from the game's own location points, so unlike every
        // layer above they carry no embedded coordinates at all. See place_live_pois.
        static constexpr int kFastTravelLayer = 1003;
        static constexpr int kTowerLayer = 1004;
        static constexpr int kSealedLayer = 1005;
        // Track-one-Pal species layer: no static data either -- spawn points are
        // joined live from the two spawner DataTables at map-open (see
        // place_tracked_pal). One layer, retargeted by the TrackPal setting.
        static constexpr int kPalSpeciesLayer = 1006;
        // The compass icon family (/Game/Pal/Texture/UI/InGame/) -- vanilla's own art
        // for exactly these points. Not codegen'd: these layers have no static data.
        static constexpr const wchar_t* kFastTravelIcon = L"T_icon_compass_FTtower";
        static constexpr const wchar_t* kTowerIcon = L"T_icon_compass_tower";
        static constexpr const wchar_t* kSealedIcon = L"T_icon_compass_BossGate";
        // Dungeon caves (Build 1.7): the entrance actors (APalDungeonEntrance), streamed near
        // the player -- census: 18 loaded; PalDungeonPointMarker=0 and the subsystem TMap=0.
        static constexpr int kDungeonLayer = 1007;
        static constexpr const wchar_t* kDungeonIcon = L"T_icon_compass_dungeon";
        // Lucky (rare "shiny") wild Pals (Build B): live scan of PalCharacter actors filtered
        // by IsRarePal, nearest-only. No static positions -- they wander + spawn/despawn.
        static constexpr int kLuckyLayer = 1008;

        static constexpr ExtraLayer kExtraLayers[] = {
            {kEffigyLayer, L"Effigies"},      {kNoteLayer, L"Notes"},   {kEggLayer, L"Eggs"},
            {kFastTravelLayer, L"FastTravel"}, {kTowerLayer, L"Tower"}, {kSealedLayer, L"SealedRealm"},
            {kPalSpeciesLayer, L"Pal"}, {kDungeonLayer, L"Dungeon"}, {kLuckyLayer, L"Lucky"}};
        // Dynamic panel label for the Pal layer ("Pal: <name>"); a member so its
        // c_str() outlives the panel build. Rebuilt in panel_items from g_track_pal.
        std::wstring m_pal_label = L"Pal";
        // In-game Pal picker (#40, redesigned 2026-07-19): pick ONE species to track,
        // by REAL localized name, from an A-Z letter-filtered list under the PALS
        // category. The trackable set is enumerated live from DT_PalWildSpawner (every
        // distinct Pal_1/2/3 name); display names come from build_pal_display(). The rows
        // are ordinary panel rows carrying RESERVED ids (below), so they ride the existing
        // m_panel_rows / poll_panel path -- no new widget pool. A species click edits the
        // STAGED tracked Pal (handle_pal_pick), applied to the map only on Apply.
        std::vector<std::wstring> m_pal_species;    // distinct trackable species (internal names, sorted)
        std::vector<std::wstring> m_pal_row_labels;  // backs the species rows' const wchar_t* (rebuilt per panel_items)
        // Reserved panel-row ids for the picker -- above every real layer id (kLayers
        // 0..~21, extra layers 1000..1006), so they never collide. kPalPagePrev is kept
        // only as the low end of the Pal-pick band (paging itself is gone).
        static constexpr int kPalPagePrev = 19990;   // band floor for is_pal_pick_id
        static constexpr int kPalPickBase = 20000;   // ..20999: species row = base + filtered index
        // Hard cap on species rows rendered at once. The wild-spawner table yields ~482
        // "species" (variants + non-pal spawner entries), and one letter clustered to ~197
        // -> a 262-row panel that spiked Slate and crashed (Kenny, 2026-07-19). A single
        // layout near ~56 rows is known safe; the rail is ~26, so cap the list well under.
        static constexpr size_t kPalListMax = 20;
        // Effigy relic-type picker ids (#43): 13 types + "All", no paging (short list).
        // A separate band from the Pal picker so poll routing is unambiguous.
        static constexpr int kRelicAll = 21000;       // "All types"
        static constexpr int kRelicPickBase = 21001;  // ..21013: id = base + EPalRelicType
        // Config-menu control ids (2026-07-19), a band above every pick id. The panel is
        // now a launcher button that opens a staged config menu; the map only re-applies
        // on Apply. These drive open/apply/cancel + the A-Z letter filter for the Pal list.
        static constexpr int kMenuOpen = 22001;    // launcher -> open the menu
        static constexpr int kMenuApply = 22002;   // commit staged edits -> map
        static constexpr int kMenuCancel = 22003;  // discard staged edits
        static constexpr int kLetterAll = 22010;   // clear the letter filter (also the "tap a letter" hint id)
        static constexpr int kTabBase = 22020;     // ..22022: config-modal tab select (Layers/Effigies/Pals)
        static constexpr int kLetterBase = 22100;  // ..22125: filter Pal list by A..Z (base + c-'A')
        static auto is_pal_pick_id(int id) -> bool { return id >= kPalPagePrev && id < kRelicAll; }
        static auto is_relic_pick_id(int id) -> bool { return id >= kRelicAll && id < 22000; }
        static auto is_menu_ctrl_id(int id) -> bool { return id >= 22000 && id < 23000; }
        static auto is_any_pick_id(int id) -> bool
        {
            return is_pal_pick_id(id) || is_relic_pick_id(id) || is_menu_ctrl_id(id);
        }
        // Dynamic label for the EFFIGIES toggle row ("Effigies: <type>"); member so its
        // c_str() outlives the panel build, like m_pal_label.
        std::wstring m_relic_label = L"Effigies";

        // --- Config-menu redesign (2026-07-19) ---
        // The always-open legend became a launcher button; clicking it opens a staged
        // config menu. Layer toggles, the tracked Pal, and the effigy filter are edited
        // in STAGED copies; the map only re-applies when Kenny hits Apply (so a dense
        // layer's churn happens once, on confirm, not per click -- also closes the
        // live-toggle crash surface). Cancel throws the staged copies away.
        bool m_menu_open = false;
        int m_active_tab = 0;   // config modal: 0 = Layers, 1 = Effigies, 2 = Pals
        std::unordered_map<int, bool> m_stage_layer_on;   // staged layer toggles (seeded from committed on open)
        std::wstring m_stage_pal;               // staged tracked species (internal name)
        int m_stage_relic = -1;                 // staged effigy-type filter
        std::wstring m_pal_filter;              // active A-Z letter filter (upper); empty = show all
        // Localized display names, internal -> display (SheepBall -> Lamball). Resolved
        // once per session via UPalMasterDataTablesUtility::GetLocalizedText; falls back
        // to a prettified internal name if the reflected call yields nothing.
        std::unordered_map<std::wstring, std::wstring> m_pal_display;
        bool m_pal_display_built = false;
        // Backs the const wchar_t* of the launcher / Apply / Cancel labels for the build's
        // lifetime (like m_pal_row_labels does for the species rows).
        std::wstring m_menu_launch_label = L"Configure";
        // The tracked-Pal dot: a bright tehAon-ish magenta, distinct from every other
        // layer colour, icon-less for now (per-species art is the next step).
        static constexpr Engine::FLinearColor_ kPalColor{1.0f, 0.30f, 0.75f, 1.0f};
        static constexpr double kPalDotPx = 22.0;
        // Only mark the nearest few spawns of the tracked Pal, not all ~400: Kenny wants
        // to be guided to the CLOSEST (compass + these markers), and dumping hundreds of
        // extra widgets into the pool destabilised Slate (crashes). The compass still
        // points to the single nearest live.
        static constexpr size_t kPalMaxDots = 15;
        // Dungeon caves: a cave teal, and a nearest-N cap so a dense pocket can't flood the
        // pool. Streaming already limits FindAllOf(PalDungeonEntrance) to nearby, this bounds it.
        static constexpr Engine::FLinearColor_ kDungeonColor{0.35f, 0.85f, 0.75f, 1.0f};
        static constexpr size_t kDungeonMaxDots = 20;
        // Lucky Pals: gold to match the golden-sparkle VFX; nearest-only ("only need to track
        // nearest", Kenny). kLuckyRangeUU = when the sound/alert fires (300 m); + hysteresis so
        // it fires once per approach, not every tick at the boundary.
        static constexpr Engine::FLinearColor_ kLuckyColor{1.0f, 0.84f, 0.20f, 1.0f};
        static constexpr double kLuckyRangeUU = 30000.0;
        static constexpr double kLuckyHystUU = 3000.0;
        struct Dot
        {
            UObject* widget;
            UObject* slot;
            const wchar_t* icon;   // nullptr = plain dot
            bool icon_applied;
            double base_size;
            int layer_id;
            bool base_hidden;   // collected effigy/note: hidden regardless of toggle
            // World position, kept alongside the canvas one. The nearest readout
            // needs true distances in world units; canvas distance would be metres
            // times the projection scale, and the transform allows an axis swap and
            // per-axis scales, so there is no single divisor that recovers metres.
            double wx, wy;
            // The layer's colour, kept per-dot because a glyph icon is tinted with
            // it. It cannot be looked up from layer_id: effigies/notes/eggs use ids
            // 1000..1002, which are not kLayers indices.
            Engine::FLinearColor_ color{};
        };
        std::vector<Dot> m_dots;                           // pooled dot widgets
        std::unordered_map<int, bool> m_layer_on;          // toggle state per layer id

        // interface panel (P2): screen-fixed rows with native checkboxes
        UObject* m_panel_canvas = nullptr;
        // The config modal's centred card canvas (a child of m_panel_canvas, rebuilt every
        // layout while the menu is open; null when closed). Its children are card-relative.
        UObject* m_card_canvas = nullptr;
        UObject* m_card_slot = nullptr;
        std::wstring m_panel_root_name;
        struct PanelRow
        {
            UObject* checkbox;
            int layer_id;
            bool last_checked;
            UObject* dist;   // right-hand "340m NE" label; nullptr if it failed to build
            UObject* card = nullptr;   // the row's background card, for in-place recolour
        };
        std::vector<PanelRow> m_panel_rows;
        bool m_panel_first_poll = true;
        // Dynamic header-label widgets ("Effigies: <type>" / "Pal: <name>"), captured at
        // layout so a selection change can retext them WITHOUT a full panel rebuild.
        // Rebuilding the whole panel on every pick churned Slate and crashed the game
        // (Kenny, clicking through effigy types); update_selection_visuals recolours the
        // affected cards + retexts these in place instead.
        UObject* m_relic_name_lbl = nullptr;
        UObject* m_pal_name_lbl = nullptr;

        // Minimap (#26) SPIKE. Always-on HUD widget parented to WBP_PlayerUI's
        // CanvasPanel_Root. Keyed on that root's full name like the panel, so a
        // rebuilt HUD (respawn, reload) rebuilds the minimap instead of orphaning
        // it. Frame-only for now: the transform and dots come after the go/no-go.
        static constexpr double kMinimapDotPx = 14.0;   // big enough for a recognisable icon
        static constexpr size_t kMinimapMaxDots = 260;
        UObject* m_minimap_canvas = nullptr;
        std::wstring m_minimap_root_name;
        bool m_minimap_logged = false;
        UObject* m_minimap_frame = nullptr;          // gold border frame behind the bg
        UObject* m_minimap_bg = nullptr;             // background image (holds the terrain render target)
        UObject* m_minimap_you = nullptr;            // centre heading arrow, rotates to yaw
        UObject* m_terrain_rt = nullptr;             // UTextureRenderTarget2D (top-down capture)
        UObject* m_terrain_actor = nullptr;          // ASceneCapture2D following the player
        UObject* m_terrain_comp = nullptr;           // its USceneCaptureComponent2D
        UObject* m_captured_material = nullptr;      // vanilla M_CapturedMaterial, wraps the RT (pinned)
        bool m_terrain_tried = false;                // init attempted (once, or after a failure)
        double m_terrain_ortho = 0.0;                // last OrthoWidth pushed (to react to zoom)
        int m_terrain_rot_log = 0;                    // one-shot: log the capture's actual look direction
        int m_capture_clean_idx = 0;                 // round-robin slot for clean_terrain_capture
        std::chrono::steady_clock::time_point m_next_capture_clean{};  // throttle the hide-sweep
        bool m_you_arrowed = false;                  // arrow texture applied yet?
        std::vector<UObject*> m_minimap_dots;        // pooled layer dots on the minimap
        std::vector<UObject*> m_minimap_dot_slots;   // their canvas slots (for repositioning)
        std::vector<const wchar_t*> m_minimap_dot_icon;   // icon each pooled dot currently shows
        std::chrono::steady_clock::time_point m_next_minimap{};
        // Set from keydown callbacks (input thread); consumed on the game thread in
        // tick(). Never mutate the g_minimap_* state off-thread -- just request.
        std::atomic<bool> m_minimap_toggle_req{false};   // F8
        std::atomic<int> m_minimap_zoom_req{0};          // F9 in (-1) / F10 out (+1), accumulates
        std::atomic<bool> m_minimap_rotate_req{false};   // F6 rotate/north-up toggle
        bool m_minimap_fast = false;   // true once the per-frame EngineTick drives the minimap
        std::chrono::steady_clock::time_point m_next_mm_frame{};   // ~30 Hz gate for the EngineTick driver
        // Heartbeat: last time the per-frame EngineTick hook actually ran. On a server the
        // hook fires during the startup/menu tick then goes silent once the world is live,
        // which froze the compass empty (it is the hook's only driver). on_update watches
        // this and re-drives the compass at ~3 Hz when the hook stalls -- the two never
        // overlap (mutually exclusive by the staleness window).
        std::chrono::steady_clock::time_point m_last_frame_hook{};
        // SEH tripwire: counts access violations the tick_map guard swallowed. Silent in
        // the normal case; if a future teardown race faults, this surfaces it in the log
        // (from the C++ layer, since the __except scope may not hold C++ objects).
        unsigned m_seh_hits = 0;
        bool m_seh_new = false;
        // Toggle debounce (server crash from smash-clicking a dense layer). A single
        // toggle of a dense layer (~700 Chest dots) is fine -- the staged reveal budgets
        // it -- but a BURST of on/off clicks generated a storm of visibility/re-place
        // churn that Slate could not survive on a server (render-thread crash; on_update
        // completed every tick). Coalesce: accumulate the changed layers and apply once,
        // ~200 ms after the LAST click. Category folds route through the same settle.
        std::vector<int> m_pending_layers;
        bool m_apply_pending = false;
        std::chrono::steady_clock::time_point m_apply_at{};
        std::chrono::steady_clock::time_point m_next_hud_scan{};   // 0.5 Hz gate for the FindAllOf HUD scan
        // Idle-skip: the last pose we actually drew the minimap for; skip the per-frame
        // arrow/dot work (and its Slate invalidation) while the player holds still.
        double m_mm_last_px = 0, m_mm_last_py = 0, m_mm_last_yaw = 0;
        bool m_mm_have_last = false;
        // Perf instrument: max build_minimap / tick_minimap cost over a ~5 s window.
        long long m_mm_build_max = 0, m_mm_tick_max = 0;
        std::chrono::steady_clock::time_point m_mm_next_log{};
        // Cached player controller for the minimap -- resolving it is a ~20 ms object-
        // array walk, so it must never be per-frame. Cleared on HUD rebuild.
        UObject* m_mm_pc = nullptr;
        bool m_mm_pc_logged = false;

        // Compass strip (#24 revival). Own HUD canvas on the same WBP_PlayerUI root as
        // the minimap, but independent (either can be on/off). One pooled marker per
        // enabled layer -- icon + distance label -- placed at its camera-relative
        // bearing. Reuses the minimap's cached-PC pose + the pre-computed m_nearest, so
        // it never walks the object array per frame.
        UObject* m_compass_canvas = nullptr;
        std::wstring m_compass_root_name;
        UObject* m_compass_backing = nullptr;   // dark bar behind the markers
        UObject* m_compass_center = nullptr;    // "straight ahead" tick at centre
        struct CompassMarker
        {
            UObject* icon = nullptr;
            UObject* icon_slot = nullptr;
            UObject* label = nullptr;
            UObject* label_slot = nullptr;
            const wchar_t* cur_icon = nullptr;   // last art applied (skip re-brushing)
            std::wstring cur_text;               // last distance string (skip re-texting)
            bool cur_glyph = false;
        };
        std::vector<CompassMarker> m_compass_markers;   // pooled, reused each rebuild
        // layer id -> its OWN marker index. A marker belongs to ONE layer for the HUD's
        // life, so its icon never flips to another layer's art as arc membership / distance
        // order churn while you turn (Kenny's "phantom effigies flashing as various icons").
        std::unordered_map<int, size_t> m_compass_marker_ix;
        std::chrono::steady_clock::time_point m_next_compass_scan{};       // HUD FindAllOf throttle
        std::chrono::steady_clock::time_point m_next_compass_recompute{};  // nearest-per-layer throttle
        std::chrono::steady_clock::time_point m_next_compass_dbg{};        // rate-limit the why-empty log
        double m_compass_last_px = 0, m_compass_last_py = 0, m_compass_last_yaw = 0;
        bool m_compass_have_last = false;
        std::atomic<bool> m_compass_toggle_req{false};   // F7
        std::atomic<bool> m_census_req{false};           // F4 -- spawner-table census (dev)
        std::atomic<bool> m_menu_toggle_req{false};      // F5 -- toggle the config popup (map-only)

        // P2: the nearest node of each layer to the player, recomputed per map open.
        // Keyed by layer id (kLayers index, or the 1000+ ids of the extra layers).
        struct Nearest
        {
            double dist_uu{};
            double bearing_deg{};   // 0 = map-north, 90 = map-east (canvas space, for the panel readout)
            // The winning node's world position + art, so the compass strip can place
            // it at a CAMERA-relative bearing (world frame, no map projection) with the
            // right item icon. The canvas bearing_deg above is map-aligned and cannot do
            // camera-relative; these can.
            double wx{}, wy{};
            const wchar_t* icon{nullptr};
            Engine::FLinearColor_ color{};
        };
        std::unordered_map<int, Nearest> m_nearest;
        // Live nearest-of-each for the actor-based compass layers (eggs/dungeons/lucky),
        // refreshed by refresh_live_nearest off its OWN FindAllOf (not the map-open pool), so the
        // compass tracks things that spawn/stream while walking WITHOUT a map open. Injected into
        // m_nearest after compute_nearest each 1 Hz.
        struct LiveNearest
        {
            double wx = 0.0, wy = 0.0, dist = 0.0;
            bool valid = false;
        };
        std::unordered_map<int, LiveNearest> m_live_nearest;   // layer id -> nearest actor
        int m_live_scan_idx = 0;                               // round-robin slot (egg/dungeon/lucky)
        std::chrono::steady_clock::time_point m_next_live_scan{};
        bool m_lucky_in_range = false;   // lucky range-alert latch (fires once per approach)
        bool m_have_player = false;
        // Registered compass markers expire after LocationDisplayTimeSec = 60 (read
        // off the register itself), so they must be re-registered inside that window
        // or the compass empties while you are walking towards the rock. 45s leaves
        // room for a slow frame without ever showing a gap.
        static constexpr int kCompassRefreshSec = 45;
        static constexpr double kCompassRangeUU = 30000.0;   // = the widgets' HiddenDistance
        static constexpr size_t kCompassMaxIcons = 10;
        // 1 m. A rock sits on its spawner: the calibration measured 7/7 copper at a
        // worst of 0.7u, so this is three orders of magnitude of slack, and still far
        // tighter than the gap between two distinct nodes.
        static constexpr double kLayerMatchUU = 100.0;
        std::chrono::steady_clock::time_point m_next_compass{};
        bool m_probed_icons = false;
        bool m_selftest_done = false;
        size_t m_last_compass_count = SIZE_MAX;
        int32_t m_last_compass_hits = -1;
        static constexpr double kDistCol = 62.0;      // px reserved for "1.2km SW"
        static constexpr double kPanelMargin = 12.0;  // inset from the screen corner

        // Category folding. A header is a checkbox (checked = open); folding hides
        // its rows and re-lays out the panel, so the height must be recomputed and
        // pushed back onto the canvas slot. Open state persists like layer toggles.
        struct PanelCat
        {
            UObject* checkbox;
            std::wstring key;   // header label; the settings key is "Cat:" + this
            bool last_checked;
        };
        std::vector<PanelCat> m_panel_cats;
        std::unordered_map<std::wstring, bool> m_cat_open;
        UObject* m_panel_slot = nullptr;   // canvas slot: holds the panel's offsets
        bool m_panel_relayout = false;     // set by poll_panel, consumed by tick
        int m_panel_builds = 0;            // >1 means we rebuilt; see build_panel
        bool m_autoload_done = false;      // dev autoload is one-shot; see try_autoload

        // ---------------------------------------------------------- settings
        //
        // Layer toggles persist across sessions. This is the mod's ONLY disk access:
        // one small text file next to its own DLL, holding one "LayerKey=0|1" line
        // per layer. No network, no registry, no process spawn -- see tools/audit.
        //
        // Keyed by layer KEY, not index, so reordering or inserting layers can never
        // silently repoint a saved toggle at the wrong data.
        auto settings_path() const -> std::wstring
        {
            HMODULE self{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&Data::kCoal), &self))
            {
                return L"";
            }
            wchar_t buf[MAX_PATH]{};
            const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return L"";
            }
            std::wstring path(buf, n);
            auto slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return L"";
            }
            path.resize(slash);   // strip "main.dll", leaving .../Mods/Lodestone/dlls

            // UE4SS loads us from <Mod>/dlls/, but that is an implementation detail --
            // nobody looks for their settings inside a folder called "dlls". Hop up to
            // the mod root when that is where we are.
            const auto tail = path.find_last_of(L"\\/");
            if (tail != std::wstring::npos && path.compare(tail + 1, std::wstring::npos, L"dlls") == 0)
            {
                path.resize(tail);
            }
            return path + L"\\settings.txt";
        }

        auto load_settings() -> void
        {
            const auto path = settings_path();
            if (path.empty())
            {
                return;
            }
            std::wifstream f(path.c_str());
            if (!f)
            {
                return;   // first run: defaults stand
            }
            std::unordered_map<std::wstring, bool> saved;
            std::unordered_map<std::wstring, std::wstring> raw;
            std::wstring line;
            while (std::getline(f, line))
            {
                const auto eq = line.find(L'=');
                if (eq == std::wstring::npos || line.empty() || line[0] == L'#')
                {
                    continue;
                }
                saved[line.substr(0, eq)] = line.substr(eq + 1).find(L'1') != std::wstring::npos;
                raw[line.substr(0, eq)] = line.substr(eq + 1);
            }

            // Dot sizing knobs. Parsed before layers so a bad value can't strand the
            // rest of the file.
            auto num = [&](const wchar_t* key, double& out, double lo, double hi) {
                auto it = raw.find(key);
                if (it == raw.end())
                {
                    return;
                }
                try
                {
                    const double v = std::stod(it->second);
                    if (v >= lo && v <= hi)
                    {
                        out = v;
                    }
                }
                catch (...)
                {
                    // leave the default; a typo in settings.txt must not break startup
                }
            };
            num(L"DotScale", g_dot_scale, 0.05, 20.0);
            num(L"DotGrowth", g_dot_growth, -1.0, 1.0);
            num(L"MinimapX", g_minimap_ox, 0.0, 4000.0);
            num(L"MinimapY", g_minimap_oy, 0.0, 4000.0);
            num(L"MinimapSize", g_minimap_px, 80.0, 600.0);
            double range_m = g_minimap_range_uu / 100.0;   // settings are in metres
            num(L"MinimapRange", range_m, 40.0, 1500.0);   // 40 m floor: keep live-capture pals small
            g_minimap_range_uu = range_m * 100.0;
            num(L"CompassWidth", g_compass_w, 200.0, 3000.0);
            num(L"CompassHeight", g_compass_h, 24.0, 120.0);
            num(L"CompassArc", g_compass_arc, 20.0, 180.0);
            num(L"CompassYawOffset", g_compass_yaw_offset, -360.0, 360.0);
            num(L"CompassLiveSec", g_compass_live_sec, 1.0, 60.0);
            num(L"MinimapTerrainHeight", g_terrain_height, 1000.0, 200000.0);
            double tsrc = g_terrain_source;
            num(L"MinimapTerrainSource", tsrc, 0.0, 9.0);
            g_terrain_source = static_cast<int>(tsrc);
            num(L"MinimapTerrainExposure", g_terrain_exposure, -10.0, 10.0);
            num(L"CompassX", g_compass_ox, 0.0, 4000.0);
            num(L"CompassY", g_compass_oy, 0.0, 4000.0);
            num(L"MenuButtonSize", g_menu_btn_size, 24.0, 96.0);
            num(L"MenuButtonX", g_menu_btn_x, 0.0, 2000.0);
            num(L"MenuButtonY", g_menu_btn_y, 0.0, 2000.0);
            if (auto it = raw.find(L"TrackPal"); it != raw.end())
            {
                std::wstring v = it->second;   // trim trailing CR/space (CRLF files)
                while (!v.empty() && (v.back() == L'\r' || v.back() == L' '))
                {
                    v.pop_back();
                }
                g_track_pal = v;
            }
            if (auto it = raw.find(L"TrackRelic"); it != raw.end())
            {
                try
                {
                    const int t = std::stoi(it->second);   // stops at trailing CR/space
                    g_track_relic = (t >= 0 && t < Data::kRelicTypeCount) ? t : -1;
                }
                catch (...)
                {
                    g_track_relic = -1;   // "All" on any malformed value
                }
            }
            int applied = 0;
            for (int i = 0; i < static_cast<int>(std::size(Data::kLayers)); ++i)
            {
                auto it = saved.find(Data::kLayers[i].key);
                if (it != saved.end())
                {
                    m_layer_on[i] = it->second;
                    ++applied;
                }
            }
            for (const auto& e : kExtraLayers)
            {
                auto it = saved.find(e.key);
                if (it != saved.end())
                {
                    m_layer_on[e.id] = it->second;
                    ++applied;
                }
            }
            int cats = 0;
            for (const auto& [key, val] : saved)
            {
                if (key.starts_with(L"Cat:"))
                {
                    m_cat_open[key.substr(4)] = val;
                    ++cats;
                }
            }
            // Dev autoload. Read from `raw` (not `saved`) because the value is a save
            // directory name, not a flag -- `saved` would reduce "AF5D9C15..." to
            // whether it happens to contain a '1'.
            if (auto it = raw.find(L"OreCompass"); it != raw.end())
            {
                g_ore_compass = (it->second == L"1");
            }
            if (auto it = raw.find(L"Minimap"); it != raw.end())
            {
                g_minimap = (it->second == L"1");
            }
            if (auto it = raw.find(L"Compass"); it != raw.end())
            {
                g_compass = (it->second == L"1");
            }
            if (auto it = raw.find(L"CompassFlipEast"); it != raw.end())
            {
                g_compass_flip_east = (it->second == L"1");
            }
            if (auto it = raw.find(L"LuckySound"); it != raw.end())
            {
                g_lucky_sound = (it->second == L"1");
            }
            if (auto it = raw.find(L"CompassAnchor"); it != raw.end() && it->second.size() == 2)
            {
                // Two-letter code: vertical (T/C/B) then horizontal (L/C/R).
                switch (it->second[0])
                {
                case L'T': g_compass_ay = 0.0; break;
                case L'C': g_compass_ay = 0.5; break;
                case L'B': g_compass_ay = 1.0; break;
                }
                switch (it->second[1])
                {
                case L'L': g_compass_ax = 0.0; break;
                case L'C': g_compass_ax = 0.5; break;
                case L'R': g_compass_ax = 1.0; break;
                }
            }
            if (auto it = raw.find(L"MinimapTerrain"); it != raw.end())
            {
                g_minimap_terrain = (it->second == L"1");
            }
            if (auto it = raw.find(L"MinimapRotate"); it != raw.end())
            {
                g_minimap_rotate = (it->second == L"1");
            }
            if (auto it = raw.find(L"MinimapAnchor"); it != raw.end() && it->second.size() == 2)
            {
                // Two-letter code: vertical (T/C/B) then horizontal (L/C/R).
                switch (it->second[0])
                {
                case L'T': g_minimap_ay = 0.0; break;
                case L'C': g_minimap_ay = 0.5; break;
                case L'B': g_minimap_ay = 1.0; break;
                }
                switch (it->second[1])
                {
                case L'L': g_minimap_ax = 0.0; break;
                case L'C': g_minimap_ax = 0.5; break;
                case L'R': g_minimap_ax = 1.0; break;
                }
            }
            if (auto it = raw.find(L"AutoLoadWorld"); it != raw.end() && !it->second.empty())
            {
                g_autoload_world = it->second;
                Output::send<LogLevel::Warning>(
                    STR("[Lodestone] DEV: AutoLoadWorld={} -- will load this save from the title\n"),
                    g_autoload_world);
            }
            Output::send<LogLevel::Default>(
                STR("[Lodestone] settings loaded: {} layers, {} categories, DotScale={} DotGrowth={}\n"),
                applied, cats, g_dot_scale, g_dot_growth);
        }

        auto save_settings() -> void
        {
            const auto path = settings_path();
            if (path.empty())
            {
                return;
            }
            std::wofstream f(path.c_str(), std::ios::trunc);
            if (!f)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] could not write settings\n"));
                return;
            }
            f << L"# Lodestone settings. Delete this file to restore defaults.\n";
            f << L"#\n";
            f << L"# Marker sizing: px = base * DotScale * zoom^DotGrowth\n";
            f << L"#   base is 21px for ore/resource layers, 27px eggs, 30px effigies+notes,\n";
            f << L"#   32px live POIs (fast travel/towers/sealed realms -- their compass art\n";
            f << L"#   carries transparent margin, so it needs a bigger box to match).\n";
            f << L"#   DotScale  0.05..20   overall size multiplier (1.5 = half again bigger)\n";
            f << L"#   DotGrowth -1..1      0 = same size at every zoom (default, recommended)\n";
            f << L"#                        >0 grows as you zoom in, <0 shrinks\n";
            f << L"DotScale=" << g_dot_scale << L"\n";
            f << L"DotGrowth=" << g_dot_growth << L"\n";
            // Write the dev autoload back, or the first checkbox click silently eats
            // it: this file is truncated and rebuilt from a fixed key list, so any key
            // not emitted here does not survive a save.
            if (!g_autoload_world.empty())
            {
                f << L"#\n";
                f << L"# DEV: auto-load this save directory from the title screen.\n";
                f << L"# Delete this line to get the normal title menu back.\n";
                f << L"AutoLoadWorld=" << g_autoload_world << L"\n";
            }
            f << L"#\n";
            f << L"# EXPERIMENTAL: put nearby ore on the in-game compass (1 = on).\n";
            f << L"# Known broken: the icons appear stacked in the middle of the compass\n";
            f << L"# instead of at the direction of the rock. Off until that is fixed.\n";
            f << L"OreCompass=" << (g_ore_compass ? 1 : 0) << L"\n";
            f << L"#\n";
            f << L"# Always-on minimap on the HUD (1 = on). Shows your on-layers as dots\n";
            f << L"# around you, north-up, ~300 m radius. Toggle in-game with F8.\n";
            f << L"Minimap=" << (g_minimap ? 1 : 0) << L"\n";
            f << L"# Minimap position: two letters, vertical then horizontal.\n";
            f << L"#   vertical T/C/B (top/centre/bottom), horizontal L/C/R (left/centre/right)\n";
            f << L"#   e.g. CL = centre-left, BR = bottom-right, CC = dead centre.\n";
            f << L"# MinimapX/Y are px margins from an edge (ignored on a centred axis).\n";
            f << L"# Restart to apply.\n";
            const wchar_t vcode = (g_minimap_ay == 0.0) ? L'T' : (g_minimap_ay == 1.0) ? L'B' : L'C';
            const wchar_t hcode = (g_minimap_ax == 0.0) ? L'L' : (g_minimap_ax == 1.0) ? L'R' : L'C';
            f << L"MinimapAnchor=" << vcode << hcode << L"\n";
            f << L"MinimapX=" << g_minimap_ox << L"\n";
            f << L"MinimapY=" << g_minimap_oy << L"\n";
            f << L"# MinimapSize = square edge in px. MinimapRange = radius in metres\n";
            f << L"# (also F9/F10 zoom in-game). Restart to apply a settings.txt change.\n";
            f << L"MinimapSize=" << g_minimap_px << L"\n";
            f << L"MinimapRange=" << (g_minimap_range_uu / 100.0) << L"\n";
            f << L"# EXPERIMENTAL: live terrain under the minimap via a top-down capture\n";
            f << L"# camera (1 = on). Spawns an actor + renders the scene twice per frame.\n";
            f << L"MinimapTerrain=" << (g_minimap_terrain ? 1 : 0) << L"\n";
            f << L"# Terrain tuning while it is being fixed: MinimapTerrainHeight = capture\n";
            f << L"# camera height in uu; MinimapTerrainSource = ESceneCaptureSource\n";
            f << L"# (2=FinalColorLDR, 0=SceneColorHDR, 7=BaseColor/unlit).\n";
            f << L"MinimapTerrainHeight=" << g_terrain_height << L"\n";
            f << L"MinimapTerrainSource=" << g_terrain_source << L"\n";
            f << L"# MinimapTerrainExposure = manual brightness dial for the lit capture\n";
            f << L"# (source 2), in EV stops: 0 = neutral, positive = brighter, negative =\n";
            f << L"# darker. Tune one value so day + night both read. Ignored by BaseColor (7).\n";
            f << L"MinimapTerrainExposure=" << g_terrain_exposure << L"\n";
            f << L"# Rotate the map with you (1, player faces up) vs north-up (0). F6 toggles.\n";
            f << L"MinimapRotate=" << (g_minimap_rotate ? 1 : 0) << L"\n";
            f << L"#\n";
            f << L"# Compass strip on the HUD (1 = on). Shows the nearest of each enabled\n";
            f << L"# layer at its camera-relative bearing -- centre = straight ahead -- with\n";
            f << L"# the item icon + distance. Toggle in-game with F7.\n";
            f << L"Compass=" << (g_compass ? 1 : 0) << L"\n";
            f << L"# Compass position: two letters, vertical then horizontal (as MinimapAnchor).\n";
            f << L"#   default TC (top-centre, under vanilla's compass bar).\n";
            const wchar_t cvcode = (g_compass_ay == 0.0) ? L'T' : (g_compass_ay == 1.0) ? L'B' : L'C';
            const wchar_t chcode = (g_compass_ax == 0.0) ? L'L' : (g_compass_ax == 1.0) ? L'R' : L'C';
            f << L"CompassAnchor=" << cvcode << chcode << L"\n";
            f << L"CompassX=" << g_compass_ox << L"\n";
            f << L"CompassY=" << g_compass_oy << L"\n";
            f << L"# CompassWidth/Height = strip size in px. CompassArc = degrees shown each\n";
            f << L"# side of centre (90 = a 180-deg forward view). Restart to apply.\n";
            f << L"CompassWidth=" << g_compass_w << L"\n";
            f << L"CompassHeight=" << g_compass_h << L"\n";
            f << L"CompassArc=" << g_compass_arc << L"\n";
            f << L"# CompassYawOffset = degrees added to the heading. If markers point the wrong\n";
            f << L"# way (walking to a centred marker moves you AWAY), set 180; ±90 rotates a\n";
            f << L"# quarter. Default 0. Restart to apply.\n";
            f << L"CompassYawOffset=" << g_compass_yaw_offset << L"\n";
            f << L"# CompassFlipEast = 1 mirrors left/right (fixes a marker that is actually NE\n";
            f << L"# showing at NW, etc). Use this for a MIRROR; CompassYawOffset for a rotation.\n";
            f << L"CompassFlipEast=" << (g_compass_flip_east ? 1 : 0) << L"\n";
            f << L"# LuckySound = 1 alerts when the nearest lucky (rare) wild Pal comes within\n";
            f << L"# ~300m. Interim is a log line; a real in-game sound comes in a follow-up.\n";
            f << L"LuckySound=" << (g_lucky_sound ? 1 : 0) << L"\n";
            f << L"# CompassLiveSec = how often eggs/dungeons/lucky refresh on the compass WITHOUT\n";
            f << L"# opening the map (seconds). Lower = snappier, higher = less CPU. Default 6.\n";
            f << L"CompassLiveSec=" << g_compass_live_sec << L"\n";
            f << L"#\n";
            f << L"# Config cog button (top-right of the open map). Size in px; X/Y nudge it\n";
            f << L"# in from the top-right corner. F5 also toggles the popup. Restart to apply.\n";
            f << L"MenuButtonSize=" << g_menu_btn_size << L"\n";
            f << L"MenuButtonX=" << g_menu_btn_x << L"\n";
            f << L"MenuButtonY=" << g_menu_btn_y << L"\n";
            f << L"#\n";
            f << L"# Track-one-Pal: the INTERNAL species name to mark on the map (needs the\n";
            f << L"# 'Pal' layer toggled on). Examples: SheepBall=Lamball, PinkCat=Cattiva,\n";
            f << L"# ChickenPal=Chikipi, BlueDragon=Azurmane. Empty = track nothing. Restart\n";
            f << L"# to apply (an in-game picker is coming).\n";
            f << L"TrackPal=" << g_track_pal << L"\n";
            f << L"TrackRelic=" << g_track_relic << L"\n";
            f << L"#\n";
            f << L"# Layer toggles.\n";
            for (int i = 0; i < static_cast<int>(std::size(Data::kLayers)); ++i)
            {
                f << Data::kLayers[i].key << L'=' << (is_layer_on(i) ? 1 : 0) << L'\n';
            }
            for (const auto& e : kExtraLayers)
            {
                f << e.key << L'=' << (is_layer_on(e.id) ? 1 : 0) << L'\n';
            }
            f << L"#\n";
            f << L"# Panel categories: 1 = expanded, 0 = folded.\n";
            for (const auto& [key, val] : m_cat_open)
            {
                f << L"Cat:" << key << L'=' << (val ? 1 : 0) << L'\n';
            }
        }

        // default_on no longer gates dot creation, so it means what it says: the
        // out-of-the-box state of each toggle. Saved settings override it.
        auto seed_layer_defaults() -> void
        {
            for (int i = 0; i < static_cast<int>(std::size(Data::kLayers)); ++i)
            {
                m_layer_on[i] = Data::kLayers[i].default_on;
            }
            // Extra layers (1000+) default ON via is_layer_on's "unknown key => true",
            // except the Pal-species layer: it would otherwise dump a whole species'
            // spawns on every map the first time, so seed it OFF (opt-in).
            m_layer_on[kPalSpeciesLayer] = false;
            // The two pickers (PALS, EFFIGIES) get tall when expanded, so default them
            // FOLDED to keep the panel compact -- the header stays visible to expand.
            // A saved Cat: state (loaded below) overrides this.
            m_cat_open[L"PALS"] = false;
            m_cat_open[L"EFFIGIES"] = false;
            load_settings();
        }

        auto is_layer_on(int layer_id) const -> bool
        {
            auto it = m_layer_on.find(layer_id);
            return it == m_layer_on.end() ? true : it->second;
        }

        // Total points of a layer, for the palworld.gg-style count badge. Static layers
        // read their embedded count; the live layers (eggs / fast travel / towers / sealed
        // realms) have no static data, so count them from the placed dot pool.
        auto layer_count(int id) const -> int
        {
            if (id >= 0 && id < static_cast<int>(std::size(Data::kLayers)))
            {
                return static_cast<int>(Data::kLayers[id].count);
            }
            if (id == kEffigyLayer)
            {
                return static_cast<int>(std::size(Data::kEffigies));
            }
            if (id == kNoteLayer)
            {
                return static_cast<int>(std::size(Data::kNotes));
            }
            int n = 0;
            for (const auto& d : m_dots)
            {
                if (d.layer_id == id)
                {
                    ++n;
                }
            }
            return n;
        }

        // How many of a layer's items are already obtained/unlocked ("found"), or -1 if the
        // layer has no completion data (ores, chests, towers, ...). Only the three layers with
        // a real in-game "collected" state report it: effigies + notes (obtained flags) and
        // fast travel (per-player unlock). Drives the found/total badge.
        auto layer_found(int id) const -> int
        {
            if (id == kEffigyLayer)
            {
                return m_eff_found;
            }
            if (id == kNoteLayer)
            {
                return m_note_found;
            }
            if (id == kFastTravelLayer)
            {
                return m_ft_found;
            }
            return -1;
        }

        // The denominator for the badge. Fast travel's layer_count is the REMAINING (locked)
        // dot count, so use the scanned total instead; every other layer uses layer_count.
        auto layer_total(int id) const -> int
        {
            if (id == kFastTravelLayer && m_ft_total >= 0)
            {
                return m_ft_total;
            }
            return layer_count(id);
        }

        // Recompute effigy/note "found" from the obtained set. Called on place AND on
        // refresh, so the badge tracks collection without a full re-place. have=false =>
        // flags unknown, leave both as -1 (badge falls back to a plain count). Counts the
        // full static set (ignoring any relic-type filter) so the badge reflects overall
        // completion, not the currently-shown subset.
        auto recompute_found(const std::unordered_set<std::wstring>& collected, bool have) -> void
        {
            if (!have)
            {
                m_eff_found = -1;
                m_note_found = -1;
                return;
            }
            int ef = 0;
            for (size_t i = 0; i < std::size(Data::kEffigies); ++i)
            {
                if (collected.contains(Collected::guid_key(Data::kEffigies[i].guid)))
                {
                    ++ef;
                }
            }
            int nf = 0;
            for (size_t i = 0; i < std::size(Data::kNotes); ++i)
            {
                if (collected.contains(std::wstring(Data::kNotes[i].row)))
                {
                    ++nf;
                }
            }
            m_eff_found = ef;
            m_note_found = nf;
        }

        // Categories start COLLAPSED (2026-07-19): the config menu can be taller than the
        // screen with several open, and there is no scroll, so the menu opens compact and
        // expands one category at a time (accordion, see poll_categories).
        auto is_cat_open(const std::wstring& label) const -> bool
        {
            auto it = m_cat_open.find(label);
            return it == m_cat_open.end() ? false : it->second;
        }

        // A panel entry: the title banner, a category header, or a toggle row.
        struct PanelItem
        {
            enum Kind
            {
                Title,
                Header,
                Row
            } kind;
            const wchar_t* label;
            int id;                // layer id (Row only)
            // Swatch (Row only): the legend, left of the checkbox. Shows the
            // layer's item icon; falls back to a colour square for layers that
            // have no icon, and for either if the textures never loaded. The
            // checkbox and label stay uniform gold -- colour belongs here, where
            // it identifies without having to stay readable as UI paint.
            const wchar_t* icon = nullptr;
            float r = 0, g = 0, b = 0;
            // Config-menu redesign (2026-07-19). A Row renders in one of four looks:
            //   Pill   -- a layer toggle (the 2-column palworld.gg pill, unchanged).
            //   Pick   -- a full-width menu item (Pal species / effigy type): flat, one
            //             per line so real names never clip; `selected` drives the
            //             highlight, NOT is_layer_on (which defaulted every picker row to
            //             "on" -> the "all highlighted" bug Kenny reported).
            //   Chip   -- a small fixed-width item that wraps into a rail (the A-Z letter
            //             filter); `selected` = the active filter.
            //   Button -- a prominent action (Configure / Apply / Cancel).
            enum Style
            {
                Pill,
                Pick,
                Chip,
                Button,
                Tab     // a top-of-modal tab header (Layers / Effigies / Pals)
            } style = Pill;
            bool selected = false;   // Pick/Chip/Button/Tab: draw the active/highlight state
            // A wide Pill spans the full card width instead of joining the 2-column grid
            // (the effigy / pal layer-toggle summary rows, which sit above their picks).
            bool wide = false;
        };

        // A toggle row for a kLayers entry, carrying what its swatch needs.
        auto layer_row(int idx) -> PanelItem
        {
            const auto& l = Data::kLayers[idx];
            // Show the real in-game item name (l.display), not the internal key -- the key
            // stays the settings.txt id + log label everywhere else.
            return {PanelItem::Row, l.display, idx, l.icon, Style::srgb8(l.r), Style::srgb8(l.g), Style::srgb8(l.b)};
        }

        // The panel model. Two modes (2026-07-19 redesign):
        //   CLOSED -- a single launcher Button. The map shows only the last-applied set;
        //             nothing here can change it until the menu is opened.
        //   OPEN   -- the config menu. Layer toggles / Pal / effigy are edited in STAGED
        //             copies; Apply commits them to the map in one budgeted pass, Cancel
        //             discards. Picker rows are flat menu items (Pick/Chip), NOT toggle
        //             pills, so they no longer all read as "on".
        auto panel_items() -> std::vector<PanelItem>
        {
            std::vector<PanelItem> v;
            if (!m_menu_open)
            {
                v.push_back({PanelItem::Title, L"Lodestone", 0});
                v.push_back({PanelItem::Row, L"Configure map", kMenuOpen, nullptr, 0.0f, 0.0f, 0.0f,
                             PanelItem::Button, false});
                return v;
            }

            // Open = the tabbed config modal. Title + tab bar, then ONLY the active tab's
            // body; Apply / Cancel go at the very bottom (appended after this switch).
            v.push_back({PanelItem::Title, L"Configure Lodestone", 0});
            {
                static const wchar_t* const kTabName[3] = {L"Layers", L"Effigies", L"Pals"};
                for (int t = 0; t < 3; ++t)
                {
                    v.push_back({PanelItem::Row, kTabName[t], kTabBase + t, nullptr, 0.0f, 0.0f, 0.0f,
                                 PanelItem::Tab, m_active_tab == t});
                }
            }

            if (m_active_tab == 0)   // LAYERS tab: the on/off toggles, grouped
            {
                v.push_back({PanelItem::Header, L"COLLECTABLES", 0});
                v.push_back({PanelItem::Row, L"Notes", kNoteLayer, Data::kNoteIcon, 0.90f, 0.02f, 0.45f});
                v.push_back({PanelItem::Row, L"Eggs (nearby)", kEggLayer, Data::kEggIcon, 1.0f, 0.82f, 0.15f});

                v.push_back({PanelItem::Header, L"ORES", 0});
                for (int idx : {0, 1, 2, 3, 4, 6, 7, 8, 9, 18})   // Coal..NightStone + Paldium
                {
                    v.push_back(layer_row(idx));
                }

                v.push_back({PanelItem::Header, L"RESOURCES", 0});
                for (int idx : {5, 10, 11, 15, 16, 19, 20, 21})   // Oil, DogCoin, Lotus, FruitTree,
                {                                                 // CaveMushroom, RedBerry, Mushroom, SkillFruit
                    v.push_back(layer_row(idx));
                }

                v.push_back({PanelItem::Header, L"POINTS OF INTEREST", 0});
                for (int idx : {12, 14, 13, 17})   // Chest, Outpost, Junk, Boss
                {
                    v.push_back(layer_row(idx));
                }
                // Live layers: read from the game's own location points, no embedded data.
                // Vanilla already draws the fast-travel points you HAVE found, so this layer
                // only adds the ones you have not (see place_live_pois).
                v.push_back({PanelItem::Row, L"Fast Travel", kFastTravelLayer, kFastTravelIcon, 0.40f, 0.85f, 1.0f});
                v.push_back({PanelItem::Row, L"Syndicate Towers", kTowerLayer, kTowerIcon, 1.0f, 0.45f, 0.85f});
                v.push_back({PanelItem::Row, L"Sealed Realms", kSealedLayer, kSealedIcon, 0.70f, 0.55f, 1.0f});
                v.push_back({PanelItem::Row, L"Dungeons", kDungeonLayer, kDungeonIcon, 0.35f, 0.85f, 0.75f});
                v.push_back({PanelItem::Row, L"Lucky Pals", kLuckyLayer, nullptr, 1.0f, 0.84f, 0.20f});
            }
            else if (m_active_tab == 1)   // EFFIGIES tab: on/off summary + a type radio
            {
                m_relic_label = (m_stage_relic < 0)
                                    ? std::wstring(L"Lifmunk Effigy: All")
                                    : (L"Lifmunk Effigy: " + std::wstring(Data::kRelicTypeName[m_stage_relic]));
                {
                    const wchar_t* sum_icon =
                        (m_stage_relic >= 0 && m_stage_relic < Data::kRelicTypeCount)
                            ? Data::kRelicTypeIcon[m_stage_relic]
                            : Data::kEffigyIcon;
                    PanelItem relic_sum{PanelItem::Row, m_relic_label.c_str(), kEffigyLayer, sum_icon,
                                        0.35f, 1.0f, 0.20f};
                    relic_sum.wide = true;   // full-width toggle above the radio
                    v.push_back(relic_sum);
                }
                v.push_back({PanelItem::Header, L"Show which type", 0});
                v.push_back({PanelItem::Row, L"All types", kRelicAll, Data::kEffigyIcon, 0.0f, 0.0f, 0.0f,
                             PanelItem::Pick, m_stage_relic < 0});
                for (int t = 0; t < Data::kRelicTypeCount; ++t)
                {
                    // Each type row shows that relic's own Pal-statue icon.
                    v.push_back({PanelItem::Row, Data::kRelicTypeName[t], kRelicPickBase + t,
                                 Data::kRelicTypeIcon[t], 1.0f, 1.0f, 1.0f, PanelItem::Pick, m_stage_relic == t});
                }
            }
            else   // m_active_tab == 2: PALS tab: on/off summary + A-Z rail + species list
            {
                m_pal_label = m_stage_pal.empty() ? std::wstring(L"Pal: none")
                                                  : (L"Pal: " + display_name(m_stage_pal));
                {
                    PanelItem pal_sum{PanelItem::Row, m_pal_label.c_str(), kPalSpeciesLayer, nullptr, kPalColor.R,
                                      kPalColor.G, kPalColor.B};
                    pal_sum.wide = true;   // full-width toggle above the rail + list
                    v.push_back(pal_sum);
                }
                build_pal_display();   // one-time enumerate + localize (needs the tables loaded)
                m_pal_row_labels.clear();
                if (m_pal_species.empty())
                {
                    m_pal_row_labels.push_back(L"(open map once to load list)");
                    {
                        PanelItem hint{PanelItem::Row, m_pal_row_labels.back().c_str(), kLetterAll, nullptr, 0.0f,
                                       0.0f, 0.0f, PanelItem::Pick, false};
                        hint.wide = true;   // a full-width hint line, not a half-grid cell
                        v.push_back(hint);
                    }
                }
                else
                {
                    static const wchar_t* const kAZ[26] = {
                        L"A", L"B", L"C", L"D", L"E", L"F", L"G", L"H", L"I", L"J", L"K", L"L", L"M",
                        L"N", L"O", L"P", L"Q", L"R", L"S", L"T", L"U", L"V", L"W", L"X", L"Y", L"Z"};
                    // Letter rail: only letters that actually have a species. Tapping one
                    // filters the list below. The list is shown ONE letter at a time (never
                    // all ~100 at once -- there is no scroll widget, so an unfiltered list
                    // would run off the screen).
                    bool has[26] = {};
                    for (const auto& s : m_pal_species)
                    {
                        const std::wstring disp = display_name(s);
                        wchar_t c = disp.empty() ? L'?' : disp[0];
                        if (c >= L'a' && c <= L'z')
                        {
                            c = static_cast<wchar_t>(c - 32);
                        }
                        if (c >= L'A' && c <= L'Z')
                        {
                            has[c - L'A'] = true;
                        }
                    }
                    for (int i = 0; i < 26; ++i)
                    {
                        if (!has[i])
                        {
                            continue;
                        }
                        const bool sel = (!m_pal_filter.empty() && m_pal_filter[0] == static_cast<wchar_t>(L'A' + i));
                        v.push_back({PanelItem::Row, kAZ[i], kLetterBase + i, nullptr, 0.0f, 0.0f, 0.0f, PanelItem::Chip,
                                     sel});
                    }
                    if (m_pal_filter.empty())
                    {
                        m_pal_row_labels.push_back(L"Tap a letter to list Pals");
                        {
                            PanelItem hint{PanelItem::Row, m_pal_row_labels.back().c_str(), kLetterAll, nullptr, 0.0f,
                                           0.0f, 0.0f, PanelItem::Pick, false};
                            hint.wide = true;
                            v.push_back(hint);
                        }
                    }
                    else
                    {
                        // Filtered species, one per line, REAL names, staged selection
                        // highlighted, CAPPED at kPalListMax so one crowded letter can't
                        // blow the layout up (the 262-row crash). The PanelItems hold const
                        // wchar_t* into m_pal_row_labels, so reserve exactly (a realloc
                        // mid-fill would dangle every earlier pointer).
                        const auto list = filtered_species();
                        const size_t shown = std::min(list.size(), kPalListMax);
                        m_pal_row_labels.reserve(shown + 1);
                        for (size_t i = 0; i < shown; ++i)
                        {
                            m_pal_row_labels.push_back(display_name(list[i]));
                            v.push_back({PanelItem::Row, m_pal_row_labels.back().c_str(),
                                         kPalPickBase + static_cast<int>(i), nullptr, 0.0f, 0.0f, 0.0f,
                                         PanelItem::Pick, list[i] == m_stage_pal});
                        }
                        if (list.size() > shown)
                        {
                            m_pal_row_labels.push_back(L"+ " + std::to_wstring(list.size() - shown) +
                                                       L" more (type to search — soon)");
                            {
                                PanelItem hint{PanelItem::Row, m_pal_row_labels.back().c_str(), kLetterAll, nullptr,
                                               0.0f, 0.0f, 0.0f, PanelItem::Pick, false};
                                hint.wide = true;
                                v.push_back(hint);
                            }
                        }
                        // Diagnostic: the wild-spawner set has ~482 entries and one letter
                        // clustered to ~197. Log the true per-letter count so the pollution
                        // can be tracked down (variants? non-pal spawner rows?).
                        Output::send<LogLevel::Default>(STR("[Lodestone] pal list: letter {} -> {} species\n"),
                                                        m_pal_filter, list.size());
                    }
                }
            }
            // Footer: Cancel + Apply side by side at the card bottom. The blank header above
            // closes any open 2-column grid so the two buttons pair on their own fresh row.
            v.push_back({PanelItem::Header, L"", 0});
            v.push_back({PanelItem::Row, L"Cancel", kMenuCancel, nullptr, 0.0f, 0.0f, 0.0f, PanelItem::Button, false});
            v.push_back({PanelItem::Row, L"Apply", kMenuApply, nullptr, 0.0f, 0.0f, 0.0f, PanelItem::Button, true});
            return v;
        }
        // A collectable dot whose visibility depends on the live obtained set.
        // key = 32-hex instance GUID (effigies) or NoteRowName (notes); both are
        // matched against Collected::gather()'s union of obtained keys.
        struct GuidDot
        {
            size_t dot_index;
            std::wstring key;
        };
        std::vector<GuidDot> m_guid_dots;                  // effigy/note dots for refresh
        // "What's left" completion counts, shown as found/total badges in the panel. -1 =
        // no data yet (flags not gathered / layer not scanned) -> badge shows a plain count.
        // Effigies/notes come from the obtained set; fast travel from the per-player unlock
        // filter in place_live_pois. Other layers have no in-game "collected" concept.
        int m_eff_found = -1;
        int m_note_found = -1;
        int m_ft_found = -1;
        int m_ft_total = -1;
        double m_applied_zoom = 1.0;
        std::optional<Project::Calibration> m_calibration;
        // The world->canvas transform is a fixed map property, but the runtime pin set is
        // unstable on a server (boss/FT pins drop out some opens). So we KEEP the best
        // calibration across map-body swaps; this flags a re-evaluation on a new body.
        bool m_recheck_cal = false;
        bool m_placed = false;
        bool m_collapsed = true;
        int m_log_budget = 20;
        int m_zoom_log_budget = 12;
        std::wstring m_last_vis_log = L"<unset>";   // log layer visibility on change, not on a budget
        int m_poll_log_budget = 8;
        bool m_logged_poll = false;
        bool m_logged_census = false;
        bool m_flag_probe_done = false;

      public:
        Mod()
        {
            ModVersion = STR("2.0.0-p1");
            ModName = STR("Lodestone");
            ModAuthors = STR("tehAon");
            ModDescription = STR("Lodestone: resources, collectables and POIs on the Palworld map");
            Output::send<LogLevel::Default>(STR("[Lodestone] loaded (P1)\n"));
        }

        auto on_unreal_init() -> void override
        {
            m_unreal_ready = true;
            // Before any dot is created or any panel row is built: defaults first,
            // then saved settings on top.
            seed_layer_defaults();
            Output::send<LogLevel::Default>(STR("[Lodestone] Unreal initialized\n"));

            // Minimap hotkeys: F8 toggle, F9 zoom in, F10 zoom out. Callbacks run on
            // UE4SS's input thread, so they only set request flags; tick() applies
            // them on the game thread.
            {
                Input::ModifierKeyArray no_mods{};
                no_mods.fill(Input::ModifierKey::MOD_KEY_START_OF_ENUM);
                register_keydown_event(Input::Key::F8, no_mods,
                                       [this]() { m_minimap_toggle_req.store(true); });
                register_keydown_event(Input::Key::F9, no_mods,
                                       [this]() { m_minimap_zoom_req.fetch_sub(1); });   // zoom in
                register_keydown_event(Input::Key::F10, no_mods,
                                       [this]() { m_minimap_zoom_req.fetch_add(1); });   // zoom out
                register_keydown_event(Input::Key::F6, no_mods,
                                       [this]() { m_minimap_rotate_req.store(true); });  // rotate/north-up
                // NOT F11 -- the OS/game grabs F11 as fullscreen and it resized the window
                // (Kenny). F6 is free and keeps the minimap keys grouped (F6-F10).
                register_keydown_event(Input::Key::F7, no_mods,
                                       [this]() { m_compass_toggle_req.store(true); });  // compass strip
                // F4: one-shot census of the spawner DataTables (Pal species + NPC
                // placements). Dev instrument -- logs to UE4SS.log, draws nothing.
                register_keydown_event(Input::Key::F4, no_mods,
                                       [this]() { m_census_req.store(true); });
                // F5: toggle the config popup while the world map is open. Input thread --
                // only touch the atomic; the game thread acts on it (see tick_map).
                register_keydown_event(Input::Key::F5, no_mods,
                                       [this]() { m_menu_toggle_req.store(true); });
            }

            // Icons must be loaded on the GAME thread, and none of the mod hooks
            // run there: on_update is UE4SS's own event loop, on_unreal_init is
            // UE4SS-InitThread, on_ui_init is the render thread. UEngine::Tick is
            // the game thread by definition -- UE4SS records GGameThreadId inside
            // its EngineTick hook -- so bounce onto it. This is the same primitive
            // Lua's ExecuteInGameThread is built on; it is not Lua-specific.
            if (!UE4SSRuntime::IsEngineTickAvailable())
            {
                Output::send<LogLevel::Warning>(
                    STR("[Lodestone] EngineTick unavailable (AOB scan missed) -- icons stay off, colored dots\n"));
                return;
            }
            Unreal::Hook::RegisterEngineTickPreCallback(
                [this](auto& data, auto*, float, bool) {
                    if (tick_load_icons())
                    {
                        data.RemoveSelf();   // done or given up; stop costing a per-frame call
                    }
                },
                {false, false, STR("Lodestone"), STR("LoadIcons")});

            // Drive the minimap per-frame so the heading arrow, rotation and terrain
            // follow are smooth -- the 300 ms on_update made them steppy. Persistent
            // (never RemoveSelf); tick() only drives the minimap if this never runs.
            Unreal::Hook::RegisterEngineTickPreCallback(
                [this](auto&, auto*, float, bool) {
                    // ~30 Hz: smooth for the arrow/rotation, but keeps the per-frame
                    // find_player_hud (a FindAllOf) and dot work off the render hot path.
                    const auto now = std::chrono::steady_clock::now();
                    if (now < m_next_mm_frame)
                    {
                        return;
                    }
                    m_next_mm_frame = now + std::chrono::milliseconds(33);
                    // Two guard layers (see minimap_frame): C++ catch keeps the hook
                    // alive on a ProcessEvent-not-available throw; SEH inside swallows an
                    // access violation from a freed object during world teardown.
                    minimap_frame();
                },
                {false, false, STR("Lodestone"), STR("Minimap")});
            m_minimap_fast = true;
        }

        auto on_update() -> void override
        {
            if (!m_unreal_ready)
            {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - m_last_tick < std::chrono::milliseconds(300))
            {
                return;
            }
            m_last_tick = now;
            tick();
        }

      private:
        auto log_once(const wchar_t* msg) -> void
        {
            if (m_log_budget > 0)
            {
                --m_log_budget;
                Output::send<LogLevel::Default>(STR("[Lodestone] {}\n"), msg);
            }
        }

        // Find the visible map body's root (screen-fixed canvas) + Canvas_MapBody
        // (pans/zooms) + Canvas_ForIcon_Mask.
        auto find_map(UObject*& out_root, UObject*& out_map_body_canvas, UObject*& out_mask_canvas) -> bool
        {
            std::vector<UObject*> bodies;
            UObjectGlobals::FindAllOf(STR("WBP_Map_Body_C"), bodies);
            UObject* best_root = nullptr;
            UObject* best_mask = nullptr;
            UObject* best_body_canvas = nullptr;
            int best_pins = 0;
            for (auto* body : bodies)
            {
                if (!body)
                {
                    continue;
                }
                Engine::ParamsIsVisible vis{};
                if (!Engine::call(body, L"IsVisible", vis) || !vis.ReturnValue)
                {
                    continue;
                }
                auto** tree = body->GetValuePtrByPropertyNameInChain<UObject*>(STR("WidgetTree"));
                if (!tree || !*tree)
                {
                    continue;
                }
                auto** root = (*tree)->GetValuePtrByPropertyNameInChain<UObject*>(STR("RootWidget"));
                if (!root || !*root)
                {
                    continue;
                }
                UObject* body_canvas = Engine::find_descendant(*root, L"Canvas_MapBody");
                UObject* mask =
                    body_canvas ? Engine::find_descendant(body_canvas, L"Canvas_ForIcon_Mask", 0) : nullptr;
                if (!mask)
                {
                    continue;
                }
                int pins = 0;
                const int32_t n = Engine::children_count(mask);
                for (int32_t i = 0; i < n; ++i)
                {
                    if (Engine::class_name(Engine::child_at(mask, i)) == L"WBP_Map_IconFTTower_C")
                    {
                        ++pins;
                    }
                }
                if (pins + 1 > best_pins)
                {
                    best_pins = pins + 1;
                    best_mask = mask;
                    best_body_canvas = body_canvas;
                    best_root = *root;
                }
            }
            out_root = best_root;
            out_map_body_canvas = best_body_canvas;
            out_mask_canvas = best_mask;
            return best_mask != nullptr;
        }

        auto read_pins(UObject* mask, const wchar_t* cls, std::vector<Project::Vec2>& out) -> void
        {
            out.clear();
            const int32_t n = Engine::children_count(mask);
            for (int32_t i = 0; i < n; ++i)
            {
                UObject* c = Engine::child_at(mask, i);
                if (Engine::class_name(c) == cls)
                {
                    double x{}, y{};
                    if (Engine::slot_position(c, x, y))
                    {
                        out.push_back({x, y});
                    }
                }
            }
        }

        auto ensure_layer_canvas(UObject* map_body_canvas, UObject* mask) -> bool
        {
            if (m_layer_canvas)
            {
                return true;
            }
            auto* canvas_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
            if (!canvas_class)
            {
                log_once(L"CanvasPanel class not found");
                return false;
            }
            // Sweep any overlay WE left on THIS body from an earlier open. Palworld
            // pools and reuses WBP_Map_Body instances (find_map picks among several),
            // and the rebuild path nulls m_layer_canvas WITHOUT detaching the old
            // overlay -- so a reused body would otherwise carry a 2nd (3rd...) overlay
            // and draw every dot twice (the "doubled effigies/fast-travel" report).
            // We only ever touch the current LIVE body here, so this is dangling-safe.
            // Named + prefix-matched: StaticConstructObject may suffix the name ("_1")
            // when a just-swept overlay still holds it pending GC.
            std::vector<UObject*> stale;
            const int32_t nchild = Engine::children_count(map_body_canvas);
            for (int32_t i = 0; i < nchild; ++i)
            {
                UObject* c = Engine::child_at(map_body_canvas, i);
                if (c && Engine::widget_name(c).starts_with(L"Lodestone_Overlay"))
                {
                    stale.push_back(c);
                }
            }
            for (UObject* s : stale)
            {
                Engine::ParamsRemoveChild rc{s, false};
                Engine::call(map_body_canvas, L"RemoveChild", rc);
            }
            if (!stale.empty())
            {
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] swept {} stale overlay(s) off a reused map body\n"), stale.size());
            }
            FStaticConstructObjectParameters params{canvas_class, map_body_canvas};
            params.Name = FName(STR("Lodestone_Overlay"));
            m_layer_canvas = UObjectGlobals::StaticConstructObject(params);
            if (!m_layer_canvas)
            {
                log_once(L"layer canvas construction failed");
                return false;
            }
            // ⚠ SPEC 2.4: our icons live ONLY in our own canvas; the game's
            // Canvas_ForIcon_Mask children are rebuilt/iterated every open and
            // foreign widgets in there crash the second open (proven).
            Engine::ParamsAddChildToCanvas add{m_layer_canvas, nullptr};
            if (!Engine::call(map_body_canvas, L"AddChildToCanvas", add) || !add.ReturnValue)
            {
                log_once(L"AddChildToCanvas(map body) failed");
                m_layer_canvas = nullptr;
                return false;
            }
            m_layer_slot = add.ReturnValue;
            Engine::ParamsSetZOrder z{100};
            Engine::call(m_layer_slot, L"SetZOrder", z);
            std::fill(std::begin(m_mask_geom), std::end(m_mask_geom), 0);
            sync_layer_geometry(mask);   // proper Set* calls: they invalidate Slate
            Output::send<LogLevel::Default>(STR("[Lodestone] layer canvas created\n"));
            return true;
        }

        // Mirror the game's mask-canvas slot geometry (zoom/layout changes)
        // using the proper slot setters so Slate invalidates immediately.
        auto sync_layer_geometry(UObject* mask) -> void
        {
            if (!m_layer_slot)
            {
                return;
            }
            auto** mask_slot = mask->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot"));
            auto* layout = (mask_slot && *mask_slot)
                               ? (*mask_slot)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("LayoutData"))
                               : nullptr;
            if (!layout)
            {
                return;
            }
            if (std::memcmp(m_mask_geom, layout, sizeof(m_mask_geom)) == 0)
            {
                return;
            }
            std::memcpy(m_mask_geom, layout, sizeof(m_mask_geom));
            const auto* margins = reinterpret_cast<const float*>(layout);        // FMargin
            const auto* anchors = reinterpret_cast<const double*>(layout + 16);  // FAnchors + alignment
            Engine::ParamsSetOffsets offs{margins[0], margins[1], margins[2], margins[3]};
            Engine::call(m_layer_slot, L"SetOffsets", offs);
            Engine::ParamsSetAnchors anch{anchors[0], anchors[1], anchors[2], anchors[3]};
            Engine::call(m_layer_slot, L"SetAnchors", anch);
            Engine::ParamsSetAlignment align{{anchors[4], anchors[5]}};
            Engine::call(m_layer_slot, L"SetAlignment", align);
        }

        // one dot: pooled construction, canvas attach, styling. Returns index or SIZE_MAX.
        //
        // `canvas` is where it draws; `world` is where it IS. Both are passed rather
        // than one derived from the other: the caller already holds the world point it
        // projected, and the nearest readout needs it back in world units.
        auto emit_dot(UClass* image_class, const Project::Vec2& canvas, const Project::Vec2& world,
                      const Engine::FLinearColor_& color, const wchar_t* icon, double base_size, bool visible,
                      int layer_id, double border_w = 0.0, const Engine::FLinearColor_* border_col = nullptr)
            -> size_t
        {
            const double px = canvas.x;
            const double py = canvas.y;
            UObject* dot = nullptr;
            if (m_emit_cursor < m_dots.size())
            {
                dot = m_dots[m_emit_cursor].widget;
            }
            else
            {
                FStaticConstructObjectParameters params{image_class, m_layer_canvas};
                dot = UObjectGlobals::StaticConstructObject(params);
                if (!dot)
                {
                    return SIZE_MAX;
                }
                Style::make_round(dot);
                m_dots.push_back(Dot{dot});
            }
            Dot& entry = m_dots[m_emit_cursor];
            const wchar_t* new_icon = g_icons_enabled ? icon : nullptr;
            // A pooled widget is reused for a DIFFERENT layer now that only the on-set is
            // emitted (bounded pool) -- but its painted brush still shows the previous
            // layer's icon. Clear icon_applied when the icon changes so paint_one repaints
            // it; otherwise, e.g., Oil dots kept the effigy icon the slot last held (Kenny).
            // Icons are string literals, so pointer compare is identity per layer.
            if (entry.icon != new_icon)
            {
                entry.icon_applied = false;
            }
            entry.icon = new_icon;
            entry.color = color;
            entry.base_size = base_size;
            entry.layer_id = layer_id;
            entry.base_hidden = !visible;
            entry.wx = world.x;
            entry.wy = world.y;
            // Icon-less dots (Notes backing) are solid discs -- round them to a true
            // circle at their size, or a big one reads as a "pink square". Icon'd dots
            // switch to Image draw type in paint_dot, so this is a no-op for them. An
            // optional ring border turns the backing into a proper map pin.
            if (!entry.icon)
            {
                if (border_w > 0.0 && border_col)
                {
                    Style::make_round(entry.widget, entry.base_size / 2.0, border_w, border_col->R,
                                      border_col->G, border_col->B, border_col->A);
                }
                else
                {
                    Style::make_round(entry.widget, entry.base_size / 2.0);
                }
            }

            Engine::ParamsAddChildToCanvas add{dot, nullptr};
            if (!Engine::call(m_layer_canvas, L"AddChildToCanvas", add) || !add.ReturnValue)
            {
                return SIZE_MAX;
            }
            entry.slot = add.ReturnValue;
            // A painted glyph keeps the layer colour; only painted item art is
            // white. An unpainted dot is the plain coloured box.
            const Engine::FLinearColor_ col_val =
                (entry.icon_applied && !Style::icon_is_glyph(entry.icon)) ? Engine::FLinearColor_{1.0f, 1.0f, 1.0f, 1.0f}
                                                                         : color;
            Engine::ParamsSetColorAndOpacity col{col_val};
            Engine::call(dot, L"SetColorAndOpacity", col);
            // Always create collapsed: apply_layer_visibility (map-open) or a toggle
            // reveals the on-layers afterwards, budgeted over frames (drain_reveals), so
            // a dense layer never flips thousands of children visible in one frame. The
            // `visible` arg still records eligibility via base_hidden above -- a collected
            // node stays hidden regardless of its layer toggle.
            Engine::ParamsSetVisibility vis{Engine::Vis_Collapsed};
            Engine::call(dot, L"SetVisibility", vis);
            Engine::ParamsSetAutoSize aut{false};
            Engine::call(entry.slot, L"SetAutoSize", aut);
            Engine::ParamsSetAlignment align{{0.5, 0.5}};
            Engine::call(entry.slot, L"SetAlignment", align);
            // Slot stays at base_size forever -- see dot_render_scale for why resizing
            // it cannot work. Zoom is handled by the render transform below.
            Engine::ParamsSetSize size{{entry.base_size, entry.base_size}};
            Engine::call(entry.slot, L"SetSize", size);
            Engine::ParamsSetRenderScale rs{{dot_render_scale(m_applied_zoom), dot_render_scale(m_applied_zoom)}};
            Engine::call(entry.widget, L"SetRenderScale", rs);
            Engine::ParamsSetPosition setpos{{px, py}};
            Engine::call(entry.slot, L"SetPosition", setpos);
            const size_t idx = m_emit_cursor++;
            auto it = m_layer_ranges.find(layer_id);
            if (it == m_layer_ranges.end())
            {
                m_layer_ranges.emplace(layer_id, std::make_pair(idx, idx + 1));
            }
            else if (it->second.second == idx)
            {
                it->second.second = idx + 1;   // still contiguous
            }
            else if (m_ranges_ok)
            {
                // A layer resumed after another one emitted. Ranges cannot describe
                // that, so stop using them everywhere rather than half-applying a
                // toggle -- which would look like the mod ignoring a checkbox.
                m_ranges_ok = false;
                Output::send<LogLevel::Warning>(
                    STR("[Lodestone] layer {} is not contiguous in the dot pool; per-layer ranges "
                        "disabled, toggles fall back to a full scan\n"),
                    layer_key(layer_id));
            }
            return idx;
        }

        // Enumerate PalEgg loot actors currently streamed in and place a dot per
        // egg (projected from its world location). Returns the count placed.
        auto place_live_eggs(UClass* image_class) -> size_t
        {
            if (!m_calibration)
            {
                return 0;
            }
            std::vector<UObject*> eggs;
            UObjectGlobals::FindAllOf(STR("PalMapObjectPalEgg"), eggs);
            size_t shown = 0;
            for (auto* egg : eggs)
            {
                double ex = 0, ey = 0, ez = 0;
                if (!egg || !Engine::actor_location(egg, ex, ey, ez) || (ex == 0 && ey == 0))
                {
                    continue;
                }
                const auto pos = m_calibration->transform.apply(ex, ey);
                if (pos.x != pos.x || pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
                {
                    continue;
                }
                if (emit_dot(image_class, pos, {ex, ey}, {1.0f, 0.82f, 0.15f, 1.0f}, Data::kEggIcon, kEggDotPx,
                             true, kEggLayer) != SIZE_MAX)
                {
                    ++shown;
                }
            }
            return shown;
        }

        // Live POI layers, straight from the game's location points.
        //
        // The census settled the gate this depended on: 174 FastTravel points are
        // registered against 22 unlocked on a day-11 save, so undiscovered points do
        // exist in the location map and these layers need no extraction, no tables and
        // no embedded coordinates.
        //
        // Fast Travel shows only LOCKED points, by design. Vanilla builds a widget for
        // every point and hides the locked ones -- which is why calibration finds >100
        // FT pins on a character who has unlocked ~20 -- so drawing all 174 would just
        // stack our dots on top of vanilla's own icons. The locked ones are the entire
        // value: they are what is left to find.
        //
        // Only FastTravel has IsUnlockMapPoint (Pal.hpp:26047); on the others the call
        // fails, which is why locked_only is per-layer rather than assumed.
        struct PoiLayer
        {
            const wchar_t* cls;
            int layer_id;
            const wchar_t* icon;
            Engine::FLinearColor_ color;
            double px;
            bool locked_only;
        };

        auto place_live_pois(UClass* image_class) -> size_t
        {
            if (!m_calibration)
            {
                return 0;
            }
            static constexpr PoiLayer kPois[] = {
                {L"PalLocationPointFastTravel", kFastTravelLayer, kFastTravelIcon,
                 {0.40f, 0.85f, 1.00f, 1.0f}, kPoiDotPx, true},
                {L"PalLocationPoint_BossTower", kTowerLayer, kTowerIcon,
                 {1.00f, 0.45f, 0.85f, 1.0f}, kPoiDotPx, false},
                {L"PalLocationPoint_StandaloneBoss", kSealedLayer, kSealedIcon,
                 {0.70f, 0.55f, 1.00f, 1.0f}, kPoiDotPx, false},
            };
            // The LOCAL player's unlocked fast-travel set (per-player, replicated) -- the real
            // filter on a dedicated server, where IsUnlockMapPoint's world flag under-reports.
            std::unordered_set<std::wstring> ft_unlocked;
            const bool have_ft = Collected::gather_ft_unlocked(ft_unlocked);
            size_t ft_by_player = 0, ft_by_worldflag = 0;   // diagnostic: which filter caught what
            size_t shown = 0;
            for (const auto& poi : kPois)
            {
                std::vector<UObject*> objs;
                UObjectGlobals::FindAllOf(poi.cls, objs);
                // Total fast-travel points (all, locked+unlocked) = the badge denominator.
                if (poi.layer_id == kFastTravelLayer && !objs.empty())
                {
                    m_ft_total = static_cast<int>(objs.size());
                }
                size_t placed = 0;
                for (auto* o : objs)
                {
                    if (!o)
                    {
                        continue;
                    }
                    if (poi.locked_only)
                    {
                        // Skip a fast-travel point the player has unlocked, by EITHER source:
                        // the world/host flag (IsUnlockMapPoint, ~22 auto-opened) OR the local
                        // player's record (FastTravelPointID in FastTravelPointUnlockFlag) --
                        // the latter is what makes it correct on a dedicated server (Kenny: our
                        // compass kept showing towers he had personally unlocked).
                        Engine::ParamsIsUnlockMapPoint u{};
                        if (Engine::call(o, L"IsUnlockMapPoint", u) && u.ReturnValue)
                        {
                            ++ft_by_worldflag;
                            continue;
                        }
                        if (have_ft)
                        {
                            auto* id = o->GetValuePtrByPropertyNameInChain<FName>(STR("FastTravelPointID"));
                            if (id && ft_unlocked.contains(id->ToString()))
                            {
                                ++ft_by_player;
                                continue;   // the local player has unlocked this one
                            }
                        }
                    }
                    Engine::ParamsGetLocation g{};
                    if (!Engine::call(o, L"GetLocation", g) ||
                        (g.ReturnValue.X == 0.0 && g.ReturnValue.Y == 0.0))
                    {
                        continue;
                    }
                    const auto pos = m_calibration->transform.apply(g.ReturnValue.X, g.ReturnValue.Y);
                    if (pos.x != pos.x || pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
                    {
                        continue;
                    }
                    if (emit_dot(image_class, pos, {g.ReturnValue.X, g.ReturnValue.Y}, poi.color, poi.icon,
                                 poi.px, true, poi.layer_id) != SIZE_MAX)
                    {
                        ++placed;
                    }
                }
                Output::send<LogLevel::Default>(STR("[Lodestone] live POI: {:<34} {:>4} of {:>4} placed\n"),
                                                poi.cls, placed, objs.size());
                shown += placed;
            }
            // Diagnostic: the player-record set size + how many FT points each filter dropped.
            // If ft_unlock_set >> worldflag on a server, the player-record source is the fix.
            Output::send<LogLevel::Default>(
                STR("[Lodestone] FT unlock: player-record set={} (have={}), dropped {} by world-flag + {} "
                    "by player-record\n"),
                ft_unlocked.size(), have_ft ? 1 : 0, ft_by_worldflag, ft_by_player);
            // Fast-travel "found" = the points we skipped as already unlocked (by world flag
            // or the player's own record). Only trust it if the FT scan actually returned points.
            if (m_ft_total > 0)
            {
                m_ft_found = static_cast<int>(ft_by_worldflag + ft_by_player);
            }
            return shown;
        }

        size_t m_emit_cursor = 0;
        // Pool index where the LIVE-layer tail begins (eggs / live POIs / tracked Pal /
        // dungeons), recorded by place_dots right before the first live emit. Everything
        // below it (static layers + effigy/note collectables) is stable across a session;
        // refresh_live_layers rewinds to here to re-scan only the moving live layers on a
        // map reopen (a collected egg despawns, a just-unlocked tower drops out).
        size_t m_live_begin = 0;
        // layer id -> [begin, end) in m_dots.
        //
        // place_dots emits one layer at a time, so each layer owns a contiguous run
        // and a toggle only has to touch its own. That turns a click from ~7.6k
        // SetVisibility calls into the size of one layer -- and parity roughly doubles
        // the pool, so the full scan is what stops scaling, not the dot count.
        //
        // The contiguity is an INVARIANT OF THE EMIT ORDER, not of anything the type
        // system enforces: interleave two layers in place_dots and a range silently
        // swallows the other layer's dots, hiding them on a toggle. So emit_dot checks
        // it rather than trusting it, and m_ranges_ok going false falls everything back
        // to the full scan -- slow and correct beats fast and wrong.
        std::unordered_map<int, std::pair<size_t, size_t>> m_layer_ranges;
        bool m_ranges_ok = true;
        // Dots pending a staged visibility flip, EITHER direction. Changing a whole dense
        // layer's visibility in one frame spikes Slate and crashed the game -- first on a
        // reveal (RedBerry, 1939 dots), then on a bulk HIDE (Junk 2757 + Chest 700 toggled
        // off together, ~3400 collapses in one frame). Hiding is not free: collapsing a
        // child still dirties the parent's layout, so a burst of collapses hits the same
        // cliff as a burst of reveals. Both paths queue here and drain_reveals applies at
        // most kRevealBudget per frame, recomputing each dot's desired state at drain time
        // (so a toggle-off before its reveal drains just collapses instead -- no stale show).
        // Lotus (700) / Mushroom (353) flipped in one frame fine, so the cap sits under that.
        std::vector<size_t> m_reveal_queue;
        static constexpr size_t kRevealBudget = 250;
        static constexpr bool g_icons_enabled = true;

        // Written by the game-thread loader, read by the UE4SS thread. m_icons_ready
        // is the release/acquire handoff that publishes m_tex_index across them;
        // after it reads true the map is immutable, so no lock is needed.
        std::unordered_map<std::wstring, UObject*> m_tex_index;
        std::atomic<bool> m_icons_ready{false};
        int m_icon_attempts = 0;   // game thread only
        static constexpr int kIconLoadAttempts = 60;
        bool m_logged_first_icon = false;   // game thread only
        bool m_logged_paint = false;

        // No icon path is embedded in the generated data: the package directory is
        // derivable from the name prefix (Style::icon_dir), verified against the
        // pak's own FullDirectoryIndex.

        // Load an icon texture through the AssetRegistry.
        //
        // This is the call the upstream mod was missing. It concluded item icons
        // were "unreachable from the C++ side (Lua object-space barrier)" and
        // shipped colored dots instead -- but it only ever *enumerated resident*
        // objects via FindAllOf, and an unloaded asset is not resident, so it
        // always found zero. There is no barrier: UE4SS's Lua LoadAsset is a thin
        // wrapper over exactly these three calls (LuaMod.cpp), and
        // UAssetRegistry.hpp documents GetAsset as "responsible for loading all
        // assets into GUObjectArray" -- the same array FindAllOf walks. One
        // process, one object space. Enumerating is not loading.
        auto load_texture_asset(const wchar_t* short_name) -> UObject*
        {
            if (!short_name || !IsInGameThread())
            {
                return nullptr;
            }
            const wchar_t* dir = Style::icon_dir(short_name);
            if (!dir)
            {
                return nullptr;   // unknown family; caller falls back to a colour square
            }
            // "/Game/dir/Name.Name" -- package path plus object name within it
            std::wstring path{dir};
            path += short_name;
            path += L'.';
            path += short_name;

            auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
            if (!registry)
            {
                return nullptr;
            }
            FAssetData data = registry->GetAssetByObjectPath(FName(path.c_str(), FNAME_Add));
            // PackageName() version-dispatches internally; do NOT use ObjectPath(),
            // which is gone in 5.1+ and only logs a warning and returns NAME_None.
            if (data.PackageName().GetComparisonIndex().IsNone())
            {
                return nullptr;   // not in the registry -- or the call itself failed
            }
            UObject* asset = UAssetRegistryHelpers::GetAsset(data);
            if (!asset)
            {
                return nullptr;
            }
            // Pin it: a freshly loaded texture is unreferenced until Slate holds
            // the brush, and GC would collect it out from under the widget.
            if (auto* item = asset->GetObjectItem())
            {
                item->SetRootSet();
                item->SetGCKeep();
            }
            return asset;
        }

        // Load any asset by its full object path ("/Game/Dir/Name.Name"), not just an
        // icon short-name routed through Style::icon_dir. Same AssetRegistry+pin path as
        // load_texture_asset. Used for the terrain material (M_CapturedMaterial).
        auto load_object_by_path(const wchar_t* object_path) -> UObject*
        {
            if (!object_path || !IsInGameThread())
            {
                return nullptr;
            }
            auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
            if (!registry)
            {
                return nullptr;
            }
            FAssetData data = registry->GetAssetByObjectPath(FName(object_path, FNAME_Add));
            if (data.PackageName().GetComparisonIndex().IsNone())
            {
                return nullptr;
            }
            UObject* asset = UAssetRegistryHelpers::GetAsset(data);
            if (!asset)
            {
                return nullptr;
            }
            if (auto* item = asset->GetObjectItem())
            {
                item->SetRootSet();
                item->SetGCKeep();
            }
            return asset;
        }

        // Resolve every icon once, on the GAME thread. Returns true when this
        // should stop being called: all resolved, or enough attempts that it is
        // clearly not going to work.
        //
        // There is deliberately no FindAllOf("Texture2D") resident-scan fallback.
        // Upstream's whole approach was that scan, and the log proved it hopeless:
        // 6333 Texture2D objects resident and not one of our 17 among them. Item
        // icons are not resident until something loads them, and the scan cannot
        // load. GetAsset returns an already-resident asset just as happily, so the
        // scan bought nothing but a 6333-object walk every retry.
        auto tick_load_icons() -> bool
        {
            if (m_icons_ready.load(std::memory_order_acquire))
            {
                return true;
            }
            ++m_icon_attempts;

            int resolved = 0, want = 0, loaded = 0;
            auto resolve_one = [&](const wchar_t* icon) {
                if (!icon)
                {
                    return;
                }
                ++want;
                if (m_tex_index.count(icon))
                {
                    ++resolved;
                    return;
                }
                if (auto* tex = load_texture_asset(icon))
                {
                    m_tex_index[icon] = tex;
                    ++resolved;
                    ++loaded;
                    if (!m_logged_first_icon)
                    {
                        m_logged_first_icon = true;
                        // What GetAsset hands back matters: the object path could
                        // resolve to the package rather than the Texture2D, and
                        // SetBrushFromTexture would silently do nothing with it.
                        auto* cls = tex->GetClassPrivate();
                        Output::send<LogLevel::Default>(STR("[Lodestone] first icon: {} (class {})\n"),
                                                        tex->GetFullName(),
                                                        cls ? cls->GetName() : STR("<null>"));
                    }
                }
            };
            for (const auto& l : Data::kLayers)
            {
                resolve_one(l.icon);
            }
            resolve_one(Data::kEffigyIcon);
            // Per-type effigy icons: each relic type is a distinct Pal statue (Lifmunk,
            // Lamball, Pengullet, ...) with its own item icon T_itemicon_Relic[_NN].
            for (const auto* relic_icon : Data::kRelicTypeIcon)
            {
                resolve_one(relic_icon);
            }
            resolve_one(Data::kNoteIcon);
            resolve_one(Data::kEggIcon);
            resolve_one(kFastTravelIcon);
            resolve_one(kTowerIcon);
            resolve_one(kSealedIcon);
            resolve_one(kDungeonIcon);
            resolve_one(L"T_icon_map_player");   // minimap heading arrow

            if (resolved == want)
            {
                // Publishes every m_tex_index write above to the UE4SS thread,
                // which only reads the map once this flag reads true.
                m_icons_ready.store(true, std::memory_order_release);
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] icons: {}/{} ready ({} loaded from pak) after {} tick(s)\n"), resolved, want,
                    loaded, m_icon_attempts);
                return true;
            }
            if (m_icon_attempts >= kIconLoadAttempts)
            {
                Output::send<LogLevel::Warning>(
                    STR("[Lodestone] icons: only {}/{} resolved after {} ticks -- the pak's directory index says "
                        "all {} assets exist, so this is the AssetRegistry call, not the paths. Colored dots.\n"),
                    resolved, want, m_icon_attempts, want);
                return true;   // give up; dots are a fine fallback
            }
            return false;   // registry may not be populated yet -- try again next frame
        }

        auto layer_texture(const wchar_t* icon) -> UObject*
        {
            if (!icon)
            {
                return nullptr;
            }
            auto it = m_tex_index.find(icon);
            if (it != m_tex_index.end() && it->second)
            {
                return it->second;
            }
            return nullptr;
        }

        // Census of the game's own POI objects. This is the go/no-go for live POI
        // layers, and it is deliberately the same data path the feature would use.
        //
        // The question is not whether UPalLocationManager exists -- the reflection
        // dump settles that -- but whether UNDISCOVERED locations are registered. If
        // only visited ones are, live POIs degrade to a "places I've been" list and
        // the coordinates have to come from the pak instead.
        //
        // No TMap and no manager needed: every point is a UObject, so FindAllOf finds
        // them directly, and UPalLocationPointStatic carries its own FVector Location
        // (CXXHeaderDump Pal.hpp:26050). That sidesteps the TMap marshalling the Lua
        // probe warned it could not do.
        //
        // Reading it: if instances >> unlocked, undiscovered points ARE registered and
        // the whole POI plan is cheap. If instances ~= unlocked, they are not.
        // IsUnlockMapPoint() exists only on FastTravel; elsewhere the call fails and
        // unlocked stays 0, which is expected and not a finding.
        // ------------------------------------------------------ dev autoload
        //
        // Click "load world" without a human. Every in-game finding this project has
        // costs a full restart -- quit, launch, wait out splash/login/title, pick a
        // world, walk somewhere, open the map -- and a person has to do all of it. So
        // the loop runs at human speed, and only while a human is awake.
        //
        // This is NOT a click simulator. UPalGameInstance exposes the same call the
        // title menu makes (SelectWorldSaveDirectoryName, Pal.hpp:22352), and the
        // game's own log names the map it opens (/Game/Pal/Maps/MainWorld_5/
        // PL_MainWorld5, after Splash -> Login -> Title). So: select, then open.
        //
        // OFF unless settings.txt has AutoLoadWorld=<save dir>. It never ships on: a
        // mod that loads a save by itself is a bug to anyone but us, which is why the
        // key is absent by default rather than false.
        auto try_autoload() -> void
        {
            if (g_autoload_world.empty() || m_autoload_done)
            {
                return;
            }
            // Only at the title. Firing mid-game would yank the player out of a world
            // -- and the title is also the only place the selection means anything.
            if (!UObjectGlobals::FindFirstOf(STR("PalGameModeTitle")))
            {
                return;
            }
            auto* gi = UObjectGlobals::FindFirstOf(STR("PalGameInstance"));
            if (!gi)
            {
                return;
            }
            m_autoload_done = true;   // one shot, whatever happens below
            Engine::ParamsSelectWorldSaveDirectoryName sel{FString(g_autoload_world.c_str())};
            if (!Engine::call(gi, L"SelectWorldSaveDirectoryName", sel) || !sel.ReturnValue)
            {
                Output::send<LogLevel::Warning>(
                    STR("[Lodestone] autoload: SelectWorldSaveDirectoryName({}) rejected\n"),
                    g_autoload_world);
                return;
            }
            auto* gs = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            if (!gs)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] autoload: no GameplayStatics\n"));
                return;
            }
            // Check the frame before writing it. The last assumed layout was wrong by
            // four bytes and would have corrupted memory silently; assuming again here
            // would be choosing not to learn that.
            UFunction* fn = gs->GetFunctionByNameInChain(FName(STR("OpenLevel"), FNAME_Find));
            if (!fn)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] autoload: OpenLevel not found\n"));
                return;
            }
            bool ok = true;
            for (FProperty* p : fn->ForEachProperty())
            {
                const auto n = p->GetName();
                const int32_t off = p->GetOffset_Internal();
                int32_t want = -1;
                if (n == STR("WorldContextObject")) want = 0x00;
                else if (n == STR("LevelName")) want = 0x08;
                else if (n == STR("bAbsolute")) want = 0x10;
                else if (n == STR("Options")) want = 0x18;
                if (want >= 0 && off != want)
                {
                    ok = false;
                    Output::send<LogLevel::Warning>(
                        STR("[Lodestone] autoload: OpenLevel param {} @ {:#04x}, expected {:#04x}\n"), n, off,
                        want);
                }
            }
            if (!ok)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] autoload: layout mismatch, NOT calling\n"));
                return;
            }
            Engine::ParamsOpenLevel op{};
            op.WorldContextObject = gi;
            op.LevelName = FName(STR("PL_MainWorld5"), FNAME_Add);
            Engine::call(gs, L"OpenLevel", op);
            Output::send<LogLevel::Default>(STR("[Lodestone] autoload: selected {}, opening PL_MainWorld5\n"),
                                            g_autoload_world);
        }

        auto census_locations() -> void
        {
            static constexpr const wchar_t* kPointClasses[] = {
                L"PalLocationPointFastTravel",     // Fast Travel
                L"PalLocationPointDungeonPortal",  // Dungeon
                L"PalLocationPoint_BossTower",     // Syndicate Tower
                L"PalLocationPoint_StandaloneBoss",// Sealed Realm
                L"PalLocationPoint_Supply",        // supply drops
                L"PalLocationPoint_TreasureMapPoint",
            };
            for (const wchar_t* cls : kPointClasses)
            {
                std::vector<UObject*> objs;
                UObjectGlobals::FindAllOf(cls, objs);
                int unlocked = 0, located = 0;
                for (auto* o : objs)
                {
                    if (!o)
                    {
                        continue;
                    }
                    Engine::ParamsIsUnlockMapPoint u{};
                    if (Engine::call(o, L"IsUnlockMapPoint", u) && u.ReturnValue)
                    {
                        ++unlocked;
                    }
                    Engine::ParamsGetLocation g{};
                    if (Engine::call(o, L"GetLocation", g) &&
                        (g.ReturnValue.X != 0.0 || g.ReturnValue.Y != 0.0))
                    {
                        ++located;
                    }
                }
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] POI census: {:<34} {:>4} instances, {:>4} with a location, {:>4} unlocked\n"),
                    cls, objs.size(), located, unlocked);
            }
        }

        // Fetch the two live spawner DataTables the game already deserialized. The
        // Get*DataTable accessors live on UPalMasterDataTablesUtility (a
        // BlueprintFunctionLibrary), NOT UPalUtility -- calling the wrong CDO makes
        // Engine::call miss the function and return null (CXXHeaderDump/Pal.hpp:29559).
        // UE4SS resolves the RowMap offset, so callers read rows at the dump's struct
        // offsets without any fragile uasset parsing.
        auto spawner_tables(UDataTable*& placement, UDataTable*& wild) -> bool
        {
            placement = nullptr;
            wild = nullptr;
            auto* util = UObjectGlobals::StaticFindObject(
                nullptr, nullptr, STR("/Script/Pal.Default__PalMasterDataTablesUtility"));
            auto* world_ctx = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (!util || !world_ctx)
            {
                return false;
            }
            struct DTArgs
            {
                UObject* WorldContextObject{};
                UObject* ReturnValue{};
            } gp{world_ctx, nullptr}, gw{world_ctx, nullptr};
            Engine::call(util, L"GetSpawnerPlacementDataTable", gp);
            Engine::call(util, L"GetWildSpawnerDataTable", gw);
            placement = static_cast<UDataTable*>(gp.ReturnValue);
            wild = static_cast<UDataTable*>(gw.ReturnValue);
            return placement && wild;
        }

        // Place a marker at every world spawn point of the currently-tracked Pal species
        // (g_track_pal). Joins the two spawner tables live at map-open: DT_PalWildSpawner
        // rows list up to 3 species (Pal_1/2/3) per SpawnerName; DT_PalSpawnerPlacement
        // rows carry a SpawnerName + world Location. So: collect the SpawnerNames whose
        // wild rows include the tracked species, then emit a dot at each placement with a
        // matching SpawnerName. Compares FNames as raw 8-byte keys (index+number) to
        // avoid ~8k ToString allocations on the placement pass. Row offsets from
        // CXXHeaderDump/Pal.hpp (placement :7730, wild :8709). Icon-less for now.
        // Nearest cave dungeons (Build 1.7). Source: FindAllOf(PalDungeonEntrance) -- the
        // entrance actors, world-partition streamed so FindAllOf returns only the ones near
        // the player (census: 18; PalDungeonPointMarker=0 and the subsystem TMap=0, both dead).
        // Cap to the nearest kDungeonMaxDots so a dense pocket can't flood the pool. Mirrors
        // place_live_pois' FindAllOf front-end + place_tracked_pal's nth_element cap. The
        // compass + nearest readout pick these up for free via compute_nearest (kDungeonLayer).
        auto place_nearest_dungeons(UClass* image_class) -> size_t
        {
            if (!m_calibration)
            {
                return 0;
            }
            std::vector<UObject*> objs;
            UObjectGlobals::FindAllOf(STR("PalDungeonEntrance"), objs);
            std::vector<std::pair<double, double>> pts;
            for (auto* o : objs)
            {
                if (!o)
                {
                    continue;
                }
                double wx = 0, wy = 0, wz = 0;
                if (!Engine::actor_location(o, wx, wy, wz) || (wx == 0.0 && wy == 0.0))
                {
                    continue;
                }
                pts.push_back({wx, wy});
            }
            const size_t total = pts.size();
            double px = 0, py = 0, pz = 0;
            if (player_pos(px, py, pz) && pts.size() > kDungeonMaxDots)
            {
                std::nth_element(pts.begin(), pts.begin() + kDungeonMaxDots, pts.end(),
                                 [px, py](const auto& a, const auto& b) {
                                     const double da = (a.first - px) * (a.first - px) +
                                                       (a.second - py) * (a.second - py);
                                     const double db = (b.first - px) * (b.first - px) +
                                                       (b.second - py) * (b.second - py);
                                     return da < db;
                                 });
                pts.resize(kDungeonMaxDots);
            }
            size_t shown = 0;
            for (const auto& [wx, wy] : pts)
            {
                const auto pos = m_calibration->transform.apply(wx, wy);
                if (pos.x != pos.x || pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
                {
                    continue;
                }
                if (emit_dot(image_class, pos, {wx, wy}, kDungeonColor, kDungeonIcon, kPoiDotPx, true,
                             kDungeonLayer) != SIZE_MAX)
                {
                    ++shown;
                }
            }
            Output::send<LogLevel::Default>(STR("[Lodestone] dungeons: {} of {} nearby placed\n"), shown, total);
            return shown;
        }

        // Scan the streamed-in PalCharacter actors, keep only the rare ("lucky") ones, and
        // return the nearest to (px,py) in world units. rare/total feed the viability probe
        // log (are lucky pals even visible on a dedicated-server client?). IsRarePal is a
        // no-arg bool getter on the replicated IndividualParameter; skip an actor whose
        // CharacterParameterComponent/IndividualParameter is still null -- it replicates a beat
        // after spawn (APalCharacter has a CheckIndividualParameterReplicate timer).
        auto find_nearest_lucky(double px, double py, double& out_wx, double& out_wy, double& out_dist,
                                int& rare, int& total) -> bool
        {
            std::vector<UObject*> chars;
            UObjectGlobals::FindAllOf(STR("PalCharacter"), chars);
            total = static_cast<int>(chars.size());
            rare = 0;
            bool have = false;
            double best_d2 = 0.0;
            for (auto* c : chars)
            {
                if (!c)
                {
                    continue;
                }
                auto** comp = c->GetValuePtrByPropertyNameInChain<UObject*>(STR("CharacterParameterComponent"));
                if (!comp || !*comp)
                {
                    continue;
                }
                auto** indiv = (*comp)->GetValuePtrByPropertyNameInChain<UObject*>(STR("IndividualParameter"));
                if (!indiv || !*indiv)
                {
                    continue;
                }
                Engine::ParamsIsRarePal r{};
                if (!Engine::call(*indiv, L"IsRarePal", r) || !r.ReturnValue)
                {
                    continue;   // player / NPC / normal pal
                }
                ++rare;
                double wx = 0, wy = 0, wz = 0;
                if (!Engine::actor_location(c, wx, wy, wz) || (wx == 0.0 && wy == 0.0))
                {
                    continue;
                }
                const double d2 = (wx - px) * (wx - px) + (wy - py) * (wy - py);
                if (!have || d2 < best_d2)
                {
                    have = true;
                    best_d2 = d2;
                    out_wx = wx;
                    out_wy = wy;
                }
            }
            if (have)
            {
                out_dist = std::sqrt(best_d2);
            }
            return have;
        }

        // Map dot for the nearest lucky Pal (called from place_dots / refresh_live_layers).
        // Nearest-only. Logs the rare/total probe so a "0 rare of N" server result is visible.
        auto place_nearest_lucky(UClass* image_class) -> size_t
        {
            if (!m_calibration)
            {
                return 0;
            }
            double px = 0, py = 0, pz = 0;
            if (!player_pos(px, py, pz))
            {
                return 0;
            }
            double wx = 0, wy = 0, dist = 0;
            int rare = 0, total = 0;
            if (!find_nearest_lucky(px, py, wx, wy, dist, rare, total))
            {
                Output::send<LogLevel::Default>(STR("[Lodestone] lucky: {} rare of {} pals (none nearby)\n"),
                                                rare, total);
                return 0;
            }
            const auto pos = m_calibration->transform.apply(wx, wy);
            if (pos.x != pos.x || pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
            {
                return 0;
            }
            const size_t idx =
                emit_dot(image_class, pos, {wx, wy}, kLuckyColor, nullptr, kPoiDotPx, true, kLuckyLayer);
            Output::send<LogLevel::Default>(STR("[Lodestone] lucky: {} rare of {} pals, nearest {:.0f}m\n"),
                                            rare, total, dist / 100.0);
            return idx != SIZE_MAX ? 1 : 0;
        }

        // Nearest actor of `class_name` to (px,py) in world units (plain FindAllOf, no filter).
        // Shared by the egg + dungeon live scans; lucky uses find_nearest_lucky (rare filter).
        auto nearest_actor(const wchar_t* class_name, double px, double py, double& out_wx,
                           double& out_wy, double& out_dist) -> bool
        {
            std::vector<UObject*> objs;
            UObjectGlobals::FindAllOf(class_name, objs);
            bool have = false;
            double best = 0.0;
            for (auto* o : objs)
            {
                if (!o)
                {
                    continue;
                }
                double wx = 0, wy = 0, wz = 0;
                if (!Engine::actor_location(o, wx, wy, wz) || (wx == 0.0 && wy == 0.0))
                {
                    continue;
                }
                const double d2 = (wx - px) * (wx - px) + (wy - py) * (wy - py);
                if (!have || d2 < best)
                {
                    have = true;
                    best = d2;
                    out_wx = wx;
                    out_wy = wy;
                }
            }
            if (have)
            {
                out_dist = std::sqrt(best);
            }
            return have;
        }

        // Refresh the compass's nearest-of-each for the ACTOR-based layers (eggs, dungeons, lucky)
        // WITHOUT opening the map -- they spawn/despawn/stream as the player walks, so the map-open
        // dot pool goes stale (Kenny: kept reopening the map every ~100m to spot new eggs). Static
        // layers (effigies/ores/notes) already track live off the pool via compute_nearest, so they
        // are not rescanned here. Round-robin: ONE FindAllOf per fire (they never stack in a frame),
        // cycling egg -> dungeon -> lucky, so each layer refreshes every ~3 fires. Results land in
        // m_live_nearest for the compass tick to inject after compute_nearest. Runs from the per-
        // frame driver, ungated by the compass toggle so the lucky alert still fires. NOT the map --
        // this touches only the small m_nearest set + the compass markers, never the map dot pool.
        auto refresh_live_nearest() -> void
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < m_next_live_scan)
            {
                return;
            }
            m_next_live_scan =
                now + std::chrono::milliseconds(std::max<long long>(200, static_cast<long long>(
                                                                             g_compass_live_sec * 1000.0 / 3.0)));
            double px = 0, py = 0, pz = 0;
            if (!player_pos(px, py, pz))
            {
                return;
            }
            const int slot = m_live_scan_idx;
            m_live_scan_idx = (m_live_scan_idx + 1) % 3;
            auto scan = [&](int layer, const wchar_t* cls) {
                if (!is_layer_on(layer))
                {
                    m_live_nearest[layer] = LiveNearest{};   // off: valid=false, injection erases it
                    return;
                }
                LiveNearest ln{};
                ln.valid = nearest_actor(cls, px, py, ln.wx, ln.wy, ln.dist);
                m_live_nearest[layer] = ln;
            };
            if (slot == 0)
            {
                scan(kEggLayer, STR("PalMapObjectPalEgg"));
            }
            else if (slot == 1)
            {
                scan(kDungeonLayer, STR("PalDungeonEntrance"));
            }
            else
            {
                // Lucky: rare filter (find_nearest_lucky) + the range-alert latch.
                if (!is_layer_on(kLuckyLayer))
                {
                    m_live_nearest[kLuckyLayer] = LiveNearest{};
                    m_lucky_in_range = false;
                }
                else
                {
                    LiveNearest ln{};
                    int rare = 0, total = 0;
                    ln.valid = find_nearest_lucky(px, py, ln.wx, ln.wy, ln.dist, rare, total);
                    m_live_nearest[kLuckyLayer] = ln;
                    const bool in_range = ln.valid && ln.dist < kLuckyRangeUU;
                    const bool out_range = !ln.valid || ln.dist > kLuckyRangeUU + kLuckyHystUU;
                    if (in_range && !m_lucky_in_range)
                    {
                        m_lucky_in_range = true;
                        if (g_lucky_sound)
                        {
                            // Interim: log only. A real in-engine cue (PlaySound2D) is Build C -- an OS
                            // beep is a USER32 import (breaks compare_dll's allowlist) + unreliable on Proton.
                            Output::send<LogLevel::Default>(
                                STR("[Lodestone] LUCKY PAL in range! {:.0f}m ({} rare of {})\n"), ln.dist / 100.0,
                                rare, total);
                        }
                    }
                    else if (out_range)
                    {
                        m_lucky_in_range = false;
                    }
                }
                // One summary per full cycle (on the lucky slot) so the refresh is visible w/o spam.
                auto dm = [this](int l) {
                    auto it = m_live_nearest.find(l);
                    return (it != m_live_nearest.end() && it->second.valid) ? it->second.dist / 100.0 : -1.0;
                };
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] live nearest (no map): egg={:.0f}m dungeon={:.0f}m lucky={:.0f}m\n"),
                    dm(kEggLayer), dm(kDungeonLayer), dm(kLuckyLayer));
            }
        }

        auto place_tracked_pal(UClass* image_class) -> size_t
        {
            if (g_track_pal.empty() || !m_calibration)
            {
                return 0;
            }
            UDataTable* placement = nullptr;
            UDataTable* wild = nullptr;
            if (!spawner_tables(placement, wild))
            {
                return 0;
            }
            // Match on FName ComparisonIndex (the 4-byte name id at offset 0 of every
            // FName -- stable regardless of the FName layout; Pal/Spawner names carry
            // Number 0, so the index alone identifies them). FNAME_Find: never mint a
            // name -- a typo'd TrackPal just finds nothing and marks nothing.
            const FName target(g_track_pal.c_str(), FNAME_Find);
            const uint32_t target_idx = *reinterpret_cast<const uint32_t*>(&target);
            std::unordered_set<uint32_t> spawner_keys;   // wild SpawnerNames that spawn it
            for (auto& kv : wild->GetRowMap())
            {
                uint8_t* row = kv.Value;
                if (!row)
                {
                    continue;
                }
                for (size_t off : {size_t{0x24}, size_t{0x44}, size_t{0x64}})   // Pal_1/2/3
                {
                    if (*reinterpret_cast<uint32_t*>(row + off) == target_idx)
                    {
                        spawner_keys.insert(*reinterpret_cast<uint32_t*>(row + 0x10));   // SpawnerName
                        break;
                    }
                }
            }
            if (spawner_keys.empty())
            {
                Output::send<LogLevel::Default>(STR("[Lodestone] tracked Pal '{}': no spawner rows\n"),
                                                g_track_pal.c_str());
                return 0;
            }
            // Collect every matching world point, then emit only the nearest kPalMaxDots
            // to the player -- the full set (~400 for a common Pal) ballooned the widget
            // pool and crashed Slate, and Kenny wants the closest ones, not all.
            std::vector<std::pair<double, double>> pts;
            for (auto& kv : placement->GetRowMap())
            {
                uint8_t* row = kv.Value;
                if (!row || !spawner_keys.contains(*reinterpret_cast<uint32_t*>(row + 0x18)))   // SpawnerName
                {
                    continue;
                }
                const double wx = *reinterpret_cast<double*>(row + 0x28);   // Location.X
                const double wy = *reinterpret_cast<double*>(row + 0x30);   // Location.Y
                if (wx != 0.0 || wy != 0.0)
                {
                    pts.push_back({wx, wy});
                }
            }
            const size_t total = pts.size();
            double px = 0, py = 0, pz = 0;
            if (player_pos(px, py, pz) && pts.size() > kPalMaxDots)
            {
                std::nth_element(pts.begin(), pts.begin() + kPalMaxDots, pts.end(),
                                 [px, py](const auto& a, const auto& b) {
                                     const double da = (a.first - px) * (a.first - px) +
                                                       (a.second - py) * (a.second - py);
                                     const double db = (b.first - px) * (b.first - px) +
                                                       (b.second - py) * (b.second - py);
                                     return da < db;
                                 });
                pts.resize(kPalMaxDots);
            }
            size_t shown = 0;
            for (const auto& [wx, wy] : pts)
            {
                const auto pos = m_calibration->transform.apply(wx, wy);
                if (pos.x != pos.x || pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
                {
                    continue;
                }
                if (emit_dot(image_class, pos, {wx, wy}, kPalColor, nullptr, kPalDotPx, true,
                             kPalSpeciesLayer) != SIZE_MAX)
                {
                    ++shown;
                }
            }
            Output::send<LogLevel::Default>(
                STR("[Lodestone] tracked Pal '{}': nearest {} of {} spawn points from {} spawner(s)\n"),
                g_track_pal.c_str(), shown, total, spawner_keys.size());
            return shown;
        }

        // Enumerate every distinct species the wild spawner table can place -- the exact
        // set a player can track -- so the panel picker (#40) lists real, findable Pals
        // rather than a hardcoded guess. Reads the same DT_PalWildSpawner used by
        // place_tracked_pal: each row lists up to 3 species at Pal_1/2/3 (0x24/0x44/0x64,
        // FNames). std::set gives A-Z order + dedup. Built once per session (cached), on
        // the UE4SS thread, only once the tables have loaded (needs a world). Cheap: one
        // pass over ~1.7k rows, ToString only on non-None slots.
        auto build_pal_species() -> void
        {
            if (!m_pal_species.empty())
            {
                return;   // cached for the session
            }
            UDataTable* placement = nullptr;
            UDataTable* wild = nullptr;
            if (!spawner_tables(placement, wild) || !wild)
            {
                return;   // tables not loaded yet; retry next layout
            }
            std::set<std::wstring> names;
            for (auto& kv : wild->GetRowMap())
            {
                uint8_t* row = kv.Value;
                if (!row)
                {
                    continue;
                }
                for (size_t off : {size_t{0x24}, size_t{0x44}, size_t{0x64}})   // Pal_1/2/3
                {
                    const auto* fn = reinterpret_cast<const FName*>(row + off);
                    if (fn->GetComparisonIndex().IsNone())
                    {
                        continue;   // empty species slot
                    }
                    std::wstring nm = fn->ToString();
                    if (!nm.empty() && nm != STR("None"))
                    {
                        names.insert(std::move(nm));
                    }
                }
            }
            m_pal_species.assign(names.begin(), names.end());
            Output::send<LogLevel::Default>(STR("[Lodestone] pal picker: {} trackable species\n"),
                                            m_pal_species.size());
        }

        // Split a camelCase internal name into words ("SheepBall" -> "Sheep Ball"). The
        // fallback display when the localized lookup is unavailable -- still far more
        // readable than the raw enum Kenny complained about.
        static auto prettify_internal(const std::wstring& s) -> std::wstring
        {
            std::wstring out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const wchar_t c = s[i];
                if (i > 0 && c >= L'A' && c <= L'Z' && s[i - 1] >= L'a' && s[i - 1] <= L'z')
                {
                    out.push_back(L' ');
                }
                out.push_back(c);
            }
            return out;
        }

        // Resolve localized Pal display names once per session (SheepBall -> Lamball) via
        // the reflected UPalMasterDataTablesUtility::GetLocalizedText(WorldContext,
        // PalMonsterName, FName). Self-instrumenting + graceful: it verifies the reflected
        // frame against our struct (the last assumed UFunction layout was off by four
        // bytes), logs how many names resolved, and falls back to prettify_internal for
        // any that come back empty -- so the picker is always readable even if the row-key
        // format needs a follow-up. The FText return is read with ToString() (the safe one).
        auto build_pal_display() -> void
        {
            if (m_pal_display_built)
            {
                return;
            }
            build_pal_species();
            if (m_pal_species.empty())
            {
                return;   // tables not loaded yet; try again next time the menu opens
            }
            m_pal_display_built = true;   // one attempt per session regardless of outcome

            // Params frame for GetLocalizedText. Natural layout; verified below before use.
            struct GLT
            {
                UObject* WorldContextObject;   // 0x00
                uint8_t TextCategory;          // 0x08  EPalLocalizeTextCategory::PalMonsterName = 4
                FName TextId;                  // 0x0C
                FText ReturnValue;             // 0x18
            };
            UObject* util = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/Pal.Default__PalMasterDataTablesUtility"));
            UObject* ctx = player_controller();
            UFunction* fn = util ? util->GetFunctionByNameInChain(FName(STR("GetLocalizedText"), FNAME_Find))
                                 : nullptr;
            bool can_call = (util && ctx && fn);
            if (fn)
            {
                for (FProperty* p : fn->ForEachProperty())
                {
                    const auto n = p->GetName();
                    const int32_t off = p->GetOffset_Internal();
                    int32_t want = -1;
                    if (n == STR("WorldContextObject")) want = static_cast<int32_t>(offsetof(GLT, WorldContextObject));
                    else if (n == STR("TextCategory")) want = static_cast<int32_t>(offsetof(GLT, TextCategory));
                    else if (n == STR("TextId")) want = static_cast<int32_t>(offsetof(GLT, TextId));
                    else if (n == STR("ReturnValue")) want = static_cast<int32_t>(offsetof(GLT, ReturnValue));
                    if (want >= 0 && off != want)
                    {
                        can_call = false;
                        Output::send<LogLevel::Warning>(
                            STR("[Lodestone] pal names: GetLocalizedText param {} @ {:#04x}, struct {:#04x}\n"), n,
                            off, want);
                    }
                }
            }
            int resolved = 0;
            for (const auto& internal : m_pal_species)
            {
                std::wstring disp;
                if (can_call)
                {
                    GLT p{};
                    p.WorldContextObject = ctx;
                    p.TextCategory = 4;   // PalMonsterName
                    p.TextId = FName(internal.c_str(), FNAME_Find);
                    util->ProcessEvent(fn, &p);
                    disp = p.ReturnValue.ToString();
                }
                if (disp.empty() || disp == STR("None"))
                {
                    disp = prettify_internal(internal);
                }
                else
                {
                    ++resolved;
                }
                m_pal_display[internal] = std::move(disp);
            }
            Output::send<LogLevel::Default>(
                STR("[Lodestone] pal names: {} of {} localized{}\n"), resolved, m_pal_species.size(),
                can_call ? STR("") : STR(" (reflected lookup unavailable -- prettified)"));
        }

        // The name to show for an internal species id: localized if resolved, else the
        // prettified internal name (never the raw enum).
        auto display_name(const std::wstring& internal) const -> std::wstring
        {
            auto it = m_pal_display.find(internal);
            return it != m_pal_display.end() ? it->second : prettify_internal(internal);
        }

        // Staged layer state while the config menu is open: seeded from the committed
        // value on first touch, so poll compares against what the menu shows, and Apply
        // can diff staged vs committed. Cleared on every open (see open_menu).
        auto stage_on(int id) -> bool
        {
            auto it = m_stage_layer_on.find(id);
            if (it != m_stage_layer_on.end())
            {
                return it->second;
            }
            const bool v = is_layer_on(id);
            m_stage_layer_on[id] = v;
            return v;
        }

        // Open the config menu: copy committed state into the staged copies, resolve
        // display names, and relayout to the menu. The map is NOT touched until Apply.
        auto open_menu() -> void
        {
            m_stage_layer_on.clear();
            m_stage_pal = g_track_pal;
            m_stage_relic = g_track_relic;
            m_pal_filter.clear();
            m_active_tab = 0;   // open on the Layers tab
            build_pal_display();
            m_menu_open = true;
            m_panel_relayout = true;
        }

        // Cancel: throw the staged edits away and collapse back to the launcher. The
        // committed state (and the map) is untouched.
        auto cancel_menu() -> void
        {
            m_menu_open = false;
            m_panel_relayout = true;
        }

        // Apply: commit the staged edits in one shot, then let the existing debounced
        // apply path (drain_pending_apply / drain_reveals) do the ONE budgeted map update.
        // This is the whole point of the redesign -- the dense-layer churn happens once,
        // on confirm, not per click.
        auto apply_menu() -> void
        {
            std::vector<int> changed;
            for (auto& kv : m_stage_layer_on)
            {
                if (is_layer_on(kv.first) != kv.second)
                {
                    m_layer_on[kv.first] = kv.second;
                    changed.push_back(kv.first);
                }
            }
            bool replace = false;
            if (m_stage_pal != g_track_pal)
            {
                g_track_pal = m_stage_pal;
                replace = true;
            }
            if (m_stage_relic != g_track_relic)
            {
                g_track_relic = m_stage_relic;
                replace = true;
            }
            save_settings();
            if (replace)
            {
                m_placed = false;   // re-run place_dots with the new Pal / effigy filter
            }
            if (!changed.empty())
            {
                for (int id : changed)
                {
                    m_pending_layers.push_back(id);
                }
                m_apply_pending = true;
                m_apply_at = std::chrono::steady_clock::now();   // no debounce: one confirmed apply
            }
            m_menu_open = false;
            m_panel_relayout = true;
        }

        // The flat-card fill for a menu row (shared by layout render + in-place recolour
        // so the two never drift). Buttons use the amber accent when active; Pick/Chip use
        // a blue highlight when selected, a dim navy otherwise.
        static auto pick_fill(bool is_button, bool sel) -> Engine::FLinearColor_
        {
            if (is_button)
            {
                return sel ? Engine::FLinearColor_{Style::kAccentR, Style::kAccentG, Style::kAccentB, 0.92f}
                           : Engine::FLinearColor_{0.16f, 0.20f, 0.28f, 0.95f};
            }
            return sel ? Engine::FLinearColor_{0.20f, 0.55f, 0.85f, 0.95f}
                       : Engine::FLinearColor_{0.10f, 0.14f, 0.20f, 0.85f};
        }

        // Reflect a STAGED selection change on the existing widgets -- recolour the pick /
        // toggle cards + retext the two headers -- WITHOUT rebuilding the panel. Rebuilding
        // (ClearChildren + ~150 fresh Slate widgets) on every pick crashed the game when
        // clicked repeatedly (Kenny, cycling effigy types). Selection changes never add or
        // remove rows, so an in-place recolour is all that is needed; only structural
        // changes (open / apply / cancel / fold / letter-filter) still relayout.
        auto update_selection_visuals() -> void
        {
            std::vector<std::wstring> pal_list;
            bool have_pal_list = false;
            for (auto& row : m_panel_rows)
            {
                if (!row.card)
                {
                    continue;
                }
                const int id = row.layer_id;
                if (is_relic_pick_id(id))
                {
                    const bool sel = (id == kRelicAll) ? (m_stage_relic < 0)
                                                       : ((id - kRelicPickBase) == m_stage_relic);
                    Engine::ParamsSetColorAndOpacity c{pick_fill(false, sel)};
                    Engine::call(row.card, L"SetColorAndOpacity", c);
                }
                else if (is_pal_pick_id(id) && id >= kPalPickBase)
                {
                    if (!have_pal_list)
                    {
                        pal_list = filtered_species();
                        have_pal_list = true;
                    }
                    const size_t idx = static_cast<size_t>(id - kPalPickBase);
                    const bool sel = (idx < pal_list.size() && pal_list[idx] == m_stage_pal);
                    Engine::ParamsSetColorAndOpacity c{pick_fill(false, sel)};
                    Engine::call(row.card, L"SetColorAndOpacity", c);
                }
                else if (is_menu_ctrl_id(id))
                {
                    // buttons + letter chips: nothing to recolour on a selection change
                }
                else
                {
                    // a real layer-toggle pill: reflect the staged on/off
                    Style::make_pill_bg(row.card, stage_on(id));
                }
            }
            if (m_relic_name_lbl)
            {
                m_relic_label = (m_stage_relic < 0)
                                    ? std::wstring(L"Lifmunk Effigy: All")
                                    : (L"Lifmunk Effigy: " + std::wstring(Data::kRelicTypeName[m_stage_relic]));
                Engine::ParamsSetText st{FText(m_relic_label.c_str())};
                Engine::call(m_relic_name_lbl, L"SetText", st);
            }
            if (m_pal_name_lbl)
            {
                m_pal_label = m_stage_pal.empty() ? std::wstring(L"Pal: none")
                                                  : (L"Pal: " + display_name(m_stage_pal));
                Engine::ParamsSetText st{FText(m_pal_label.c_str())};
                Engine::call(m_pal_name_lbl, L"SetText", st);
            }
        }

        // The trackable species matching the active letter filter, sorted by DISPLAY
        // name (so the list reads A-Z by the name Kenny sees, not the internal enum).
        // Layout AND handle_pal_pick both call this, so a picked row's index maps back
        // to the same species. Empty filter = every species.
        auto filtered_species() const -> std::vector<std::wstring>
        {
            std::vector<std::wstring> out;
            for (const auto& s : m_pal_species)
            {
                if (!m_pal_filter.empty())
                {
                    const std::wstring disp = display_name(s);
                    wchar_t first = disp.empty() ? L'?' : disp[0];
                    if (first >= L'a' && first <= L'z')
                    {
                        first = static_cast<wchar_t>(first - 32);   // uppercase
                    }
                    if (std::wstring(1, first) != m_pal_filter)
                    {
                        continue;
                    }
                }
                out.push_back(s);
            }
            std::sort(out.begin(), out.end(),
                      [this](const std::wstring& a, const std::wstring& b) { return display_name(a) < display_name(b); });
            return out;
        }

        // Route a species pick. Edits the STAGED tracked Pal only -- the map changes on
        // Apply, not here. Returns true if the id was a Pal-pick id.
        auto handle_pal_pick(int id) -> bool
        {
            if (!is_pal_pick_id(id))
            {
                return false;
            }
            if (id >= kPalPickBase)
            {
                const auto list = filtered_species();
                const size_t sel = static_cast<size_t>(id - kPalPickBase);
                if (sel < list.size())
                {
                    m_stage_pal = list[sel];
                    m_stage_layer_on[kPalSpeciesLayer] = true;   // stage the layer on so Apply shows it
                }
            }
            return true;
        }

        // Route an effigy relic-type pick. Edits the STAGED filter only; the map changes
        // on Apply. Returns true if id was a relic-picker id.
        auto handle_relic_pick(int id) -> bool
        {
            if (!is_relic_pick_id(id))
            {
                return false;
            }
            if (id == kRelicAll)
            {
                m_stage_relic = -1;
            }
            else
            {
                const int t = id - kRelicPickBase;
                if (t >= 0 && t < Data::kRelicTypeCount)
                {
                    m_stage_relic = t;
                }
            }
            m_stage_layer_on[kEffigyLayer] = true;   // stage effigies on so Apply shows the pick
            return true;
        }

        // Route a config-menu control (launcher / Apply / Cancel / A-Z letter filter).
        // Returns true if the id was a menu-control id.
        auto handle_menu_ctrl(int id) -> bool
        {
            if (!is_menu_ctrl_id(id))
            {
                return false;
            }
            if (id == kMenuOpen)
            {
                open_menu();
            }
            else if (id == kMenuApply)
            {
                apply_menu();
            }
            else if (id == kMenuCancel)
            {
                cancel_menu();
            }
            else if (id >= kTabBase && id < kTabBase + 3)
            {
                // Switch config-modal tab. Changes the row SET, so relayout (poll_panel
                // returns right after, since this destroys the widgets it is iterating).
                m_active_tab = id - kTabBase;
                m_pal_filter.clear();   // a fresh tab starts with no letter filter
                m_panel_relayout = true;
            }
            else if (id == kLetterAll)
            {
                m_pal_filter.clear();
                m_panel_relayout = true;
            }
            else if (id >= kLetterBase && id < kLetterBase + 26)
            {
                m_pal_filter = std::wstring(1, static_cast<wchar_t>(L'A' + (id - kLetterBase)));
                m_panel_relayout = true;
            }
            return true;
        }

        // Live census of the two spawner DataTables -- the go/no-go for the Pal-species
        // and merchant layers, the same instrument-first move that settled the POI gate
        // (#7). Reads the tables the game already deserialized: UPalUtility hands back a
        // UDataTable and UE4SS resolves the RowMap offset, so no fragile uasset parsing.
        // Runs inside the minimap frame's SEH guard, so a stray read is swallowed, not a
        // crash. All field offsets are from CXXHeaderDump/Pal.hpp (the row structs at
        // :7730 placement, :8709 wild) -- FName is 8 B but 4-aligned inside the rows.
        auto census_spawners() -> void
        {
            UDataTable* placement = nullptr;
            UDataTable* wild = nullptr;
            if (!spawner_tables(placement, wild))
            {
                Output::send<LogLevel::Warning>(
                    STR("[Lodestone] spawner census: tables not loaded (util/PC/table missing)\n"));
                return;
            }

            // wild table: join on the SpawnerName COLUMN (not the row-map key). A spawner
            // can carry several weighted entries, so accumulate all Pal_N/NPC_N per name.
            auto fstr = [](uint8_t* base, size_t off) -> std::wstring {
                return reinterpret_cast<FName*>(base + off)->ToString();
            };
            std::unordered_map<std::wstring, std::wstring> wild_by_name;
            int wild_rows = 0;
            for (auto& kv : wild->GetRowMap())
            {
                uint8_t* row = kv.Value;
                if (!row)
                {
                    continue;
                }
                ++wild_rows;
                const std::wstring sname = fstr(row, 0x10);   // SpawnerName column
                std::wstring& acc = wild_by_name[sname];
                auto add = [&](size_t pal_off, size_t npc_off) {
                    const std::wstring pal = fstr(row, pal_off);
                    const std::wstring npc = fstr(row, npc_off);
                    if (pal != L"None")
                    {
                        acc += (acc.empty() ? L"" : L",") + pal;
                    }
                    if (npc != L"None")
                    {
                        acc += (acc.empty() ? L"" : L",") + std::wstring(L"NPC:") + npc;
                    }
                };
                add(0x24, 0x2C);   // Pal_1 / NPC_1
                add(0x44, 0x4C);   // Pal_2 / NPC_2
                add(0x64, 0x6C);   // Pal_3 / NPC_3
            }

            // placement table: type histogram + join outcome, plus a sample of rows.
            int counts[9] = {0};
            int placement_rows = 0, joined = 0, with_pal = 0, with_npc = 0, unjoined = 0, sample = 0;
            for (auto& kv : placement->GetRowMap())
            {
                uint8_t* row = kv.Value;
                if (!row)
                {
                    continue;
                }
                ++placement_rows;
                const uint8_t stype = *(row + 0x20);
                if (stype < 9)
                {
                    ++counts[stype];
                }
                const std::wstring sname = fstr(row, 0x18);   // SpawnerName column
                const double x = *reinterpret_cast<double*>(row + 0x28);
                const double y = *reinterpret_cast<double*>(row + 0x30);
                auto it = wild_by_name.find(sname);
                const bool has = (it != wild_by_name.end());
                if (has)
                {
                    ++joined;
                    (it->second.find(L"NPC:") != std::wstring::npos) ? ++with_npc : ++with_pal;
                }
                else
                {
                    ++unjoined;
                }
                if (sample < 24)
                {
                    ++sample;
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone]   place[{:>3}] t={} '{}' @({:.0f},{:.0f}) -> {}\n"), placement_rows,
                        stype, sname.c_str(), x, y, (has ? it->second : std::wstring(L"(no wild row)")).c_str());
                }
            }
            Output::send<LogLevel::Default>(
                STR("[Lodestone] spawner census: placement={} rows, wild={} rows; joined={} (pal={} npc={}) "
                    "unjoined={}\n"),
                placement_rows, wild_rows, joined, with_pal, with_npc, unjoined);
            Output::send<LogLevel::Default>(
                STR("[Lodestone] placement type histogram: Common={} Rare={} FieldBoss={} RandDunBoss={} "
                    "ImprisBoss={} TowerBoss={} RaidBoss={} RaidServant={} Predator={}\n"),
                counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7], counts[8]);

            // Dungeons are NOT location points (#7 census: DungeonPortal=0), so probe the
            // two live sources for a "Dungeon" POI layer (Kenny asked). (a) the placed
            // entrance-marker actors via FindAllOf (streaming-gated -> may be partial),
            // (b) the subsystem's persistent MarkerPointDataMap (TMap<FGuid,
            // FPalDungeonMarkerPointData> @0x88, Pal.hpp:21271; complete if reachable).
            // Approx map count = the TSparseArray ArrayNum at +0x08 of the map property.
            {
                std::vector<UObject*> dmark, dent;
                UObjectGlobals::FindAllOf(STR("PalDungeonPointMarker"), dmark);
                UObjectGlobals::FindAllOf(STR("PalDungeonEntrance"), dent);
                int dloc = 0;
                for (auto* m : dmark)
                {
                    double x = 0, y = 0, z = 0;
                    if (m && Engine::actor_location(m, x, y, z) && (x != 0.0 || y != 0.0))
                    {
                        ++dloc;
                    }
                }
                int map_num = -1;
                if (auto* sub = UObjectGlobals::FindFirstOf(STR("PalDungeonWorldSubsystem")))
                {
                    if (auto* mp = sub->GetValuePtrByPropertyNameInChain<uint8_t>(STR("MarkerPointDataMap")))
                    {
                        map_num = *reinterpret_cast<const int32_t*>(mp + 0x08);
                    }
                }
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] dungeon census: {} PalDungeonPointMarker ({} located), {} "
                        "PalDungeonEntrance, MarkerPointDataMap~={}\n"),
                    dmark.size(), dloc, dent.size(), map_num);
            }
        }

        // Where the player is, and what the nearest node of every layer is.
        //
        // This exists because the game's in-game map coordinates CANNOT be converted
        // to world coordinates from the reflection dump -- the conversion is blueprint
        // math and nothing in the 2,958 headers exposes it. So a report like "I found
        // quartz at map (214,-131)" cannot be checked against our data by arithmetic;
        // any formula for it is a guess. Reading the player's real world position
        // turns that guess into a measurement, and doubles as the groundwork for the
        // nearest-readout and compass work.
        auto log_player_context() -> void
        {
            double px = 0, py = 0, pz = 0;
            if (!player_pos(px, py, pz))
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] player probe: no player position\n"));
                return;
            }
            Output::send<LogLevel::Default>(STR("[Lodestone] player at world ({:.0f}, {:.0f}, {:.0f})\n"), px, py,
                                            pz);
            compute_nearest(px, py);
            // Log the closest few, closest layer first: whatever you are standing on
            // should be at the top with a small distance. This is the panel readout's
            // own check -- if a row disagrees with this line, the panel is wrong.
            // Do NOT name this `near`: <Windows.h> still defines near/far as empty
            // macros (the 16-bit segment keywords), so `near.emplace_back(...)`
            // silently becomes `.emplace_back(...)` and MSVC blames std::sort.
            // Same family as the NOMINMAX trap.
            std::vector<std::pair<double, int>> closest;
            closest.reserve(m_nearest.size());
            for (const auto& [id, n] : m_nearest)
            {
                closest.emplace_back(n.dist_uu, id);
            }
            std::sort(closest.begin(), closest.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < closest.size() && i < 6; ++i)
            {
                Output::send<LogLevel::Default>(STR("[Lodestone]   nearest {:<12} {:>9.0f}u {}\n"),
                                                layer_key(closest[i].second), closest[i].first,
                                                compass_8(m_nearest[closest[i].second].bearing_deg));
            }
            // On a reopen the panel already exists and is not rebuilt, so nothing else
            // would push the new numbers into it. (On the first open the rows do not
            // exist yet and this is a no-op; layout_panel refreshes once it builds
            // them.)
            refresh_nearest();
            if (g_ore_compass)
            {
                update_ore_compass(px, py, pz);
                m_next_compass = std::chrono::steady_clock::now() + std::chrono::seconds(kCompassRefreshSec);
            }
        }

        // The nearest UNCOLLECTED, currently-placed node of every layer.
        //
        // Reads the dot pool rather than Data::kLayers, which buys three things the
        // static tables cannot: the live layers (fast travel / towers / sealed realms
        // / eggs) have no static data at all; collected effigies and notes drop out
        // via base_hidden, so the readout means "nearest one you still need"; and
        // off-map points are already filtered. Cost is one pass over ~7.6k dots on
        // map open, next to a ~140ms placement -- not worth a spatial index.
        auto compute_nearest(double px, double py) -> void
        {
            m_nearest.clear();
            m_have_player = false;
            if (!m_calibration)
            {
                return;
            }
            // Bearing is taken in CANVAS space, not world space, so it always agrees
            // with the map the player is looking at: the projection may swap axes, and
            // then "north" in world coords is not up on screen. Canvas +y is down
            // (UMG), hence the negation.
            const auto pc = m_calibration->transform.apply(px, py);
            const size_t placed = std::min(m_emit_cursor, m_dots.size());
            for (size_t i = 0; i < placed; ++i)
            {
                const Dot& d = m_dots[i];
                if (d.base_hidden)
                {
                    continue;   // already collected: not something you still need
                }
                const double dx = d.wx - px;
                const double dy = d.wy - py;
                const double dist = std::sqrt(dx * dx + dy * dy);
                auto it = m_nearest.find(d.layer_id);
                if (it != m_nearest.end() && it->second.dist_uu <= dist)
                {
                    continue;
                }
                const auto q = m_calibration->transform.apply(d.wx, d.wy);
                double deg = std::atan2(q.x - pc.x, -(q.y - pc.y)) * 180.0 / 3.14159265358979323846;
                if (deg < 0.0)
                {
                    deg += 360.0;
                }
                m_nearest[d.layer_id] = Nearest{dist, deg, d.wx, d.wy, d.icon, d.color};
            }
            m_have_player = true;
        }

        static auto compass_8(double deg) -> const wchar_t*
        {
            static constexpr const wchar_t* kNames[] = {L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW"};
            return kNames[static_cast<size_t>(std::lround(deg / 45.0)) & 7u];
        }

        // Layer id -> display key, for logging. kLayers is indexed by id; the extra
        // layers (1000+) are not, hence the table.
        static auto layer_key(int id) -> const wchar_t*
        {
            if (id >= 0 && static_cast<size_t>(id) < std::size(Data::kLayers))
            {
                return Data::kLayers[id].key;
            }
            for (const auto& e : kExtraLayers)
            {
                if (e.id == id)
                {
                    return e.key;
                }
            }
            return L"?";
        }

        // "340m NE" / "1.2km SW" -- metres because 1 unreal unit is 1 cm.
        static auto nearest_text(const Nearest& n) -> std::wstring
        {
            const double m = n.dist_uu / 100.0;
            wchar_t buf[32];
            if (m >= 1000.0)
            {
                std::swprintf(buf, std::size(buf), L"%.1fkm %s", m / 1000.0, compass_8(n.bearing_deg));
            }
            else
            {
                std::swprintf(buf, std::size(buf), L"%.0fm %s", m, compass_8(n.bearing_deg));
            }
            return buf;
        }

        // ProcessEvent copies our param struct into the frame RAW: if our offsets are
        // wrong it corrupts the frame instead of failing, so a mismatch must SKIP the
        // call rather than risk it. This is not paranoia -- it caught RequestPlayerUId
        // sitting at 0x2C, not the 0x30 I had assumed (FGuid is four uint32, so it
        // aligns to 4 and packs straight after the float).
        //
        // Checked once: the layout is a property of the build, not of the call.
        static auto search_params_ok(UObject* searcher) -> bool
        {
            static int cached = -1;   // -1 unknown, 0 bad, 1 good
            if (cached >= 0)
            {
                return cached == 1;
            }
            UFunction* fn = searcher->GetFunctionByNameInChain(FName(STR("SearchMapObjects"), FNAME_Find));
            if (!fn)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] compass: SearchMapObjects not found\n"));
                cached = 0;
                return false;
            }
            bool ok = true;
            for (FProperty* p : fn->ForEachProperty())
            {
                const auto name = p->GetName();
                const int32_t off = p->GetOffset_Internal();
                int32_t want = -1;
                if (name == STR("SearchMapObjIds")) want = 0x00;
                else if (name == STR("Origin")) want = 0x10;
                else if (name == STR("SearchRadius")) want = 0x28;
                else if (name == STR("RequestPlayerUId")) want = 0x2C;
                if (want >= 0 && off != want)
                {
                    ok = false;
                    Output::send<LogLevel::Warning>(
                        STR("[Lodestone] compass: param {} @ {:#04x}, expected {:#04x}  <-- MISMATCH\n"), name,
                        off, want);
                }
            }
            if (!ok)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] compass: param layout mismatch, NOT calling\n"));
            }
            cached = ok ? 1 : 0;
            return ok;
        }

        // The PalPlayerController. Deliberately NOT cached across frames: a stale
        // pointer to a freed controller access-violates on world teardown (the crash on
        // leaving a server), whereas FindFirstOf just returns null there and the minimap
        // bails safely. The perf win is the throttled HUD FindAllOf, not this.
        auto player_controller() -> UObject*
        {
            return UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        }

        // Player world position, or false when there is no player to ask (title
        // screen, loading). Shared by the nearest readout and the compass, which want
        // it at different moments: one on map open, one on a timer with the map shut.
        auto player_pos(double& px, double& py, double& pz) -> bool
        {
            UObject* pc = player_controller();
            if (!pc)
            {
                return false;
            }
            Engine::ParamsGetPawn gp{};
            if (!Engine::call(pc, L"K2_GetPawn", gp) || !gp.ReturnValue)
            {
                return false;
            }
            return Engine::actor_location(gp.ReturnValue, px, py, pz);
        }

        // Player facing (world yaw degrees), for the minimap's heading arrow. Same
        // pawn as player_pos, one extra reflected call.
        auto player_yaw(double& yaw) -> bool
        {
            UObject* pc = player_controller();
            if (!pc)
            {
                return false;
            }
            Engine::ParamsGetPawn gp{};
            if (!Engine::call(pc, L"K2_GetPawn", gp) || !gp.ReturnValue)
            {
                return false;
            }
            Engine::ParamsGetActorRotation gr{};
            if (!Engine::call(gp.ReturnValue, L"K2_GetActorRotation", gr))
            {
                return false;
            }
            yaw = gr.ReturnValue.Yaw;
            return true;
        }

        // Position AND facing for the per-frame minimap, with NO object-array walk in
        // the common path: resolve the controller via GetOwningPlayer on the cached
        // HUD canvas (see below), then one K2_GetPawn and read both location and
        // rotation off the pawn. Calling player_pos + player_yaw used to do two full
        // FindFirstOf walks per frame; even one walk/frame was the minimap's lag.
        // has_yaw is false when position succeeded but the rotation read did not.
        auto player_pose(double& px, double& py, double& pz, double& yaw, bool& has_yaw) -> bool
        {
            has_yaw = false;
            // The killer measured by the perf instrument: resolving the controller is a
            // FindFirstOf/GetOwningPlayer that ends up walking Palworld's huge object
            // array (~20 ms EACH). GetOwningPlayer off the canvas returns null on this
            // build, so player_pose was falling back to FindFirstOf EVERY frame -> ~28 ms
            // ticks. Cache the PC and reuse it: a cached PC is validated cheaply by the
            // K2_GetPawn below (a reflected call, not a walk); only re-resolve when that
            // fails (world change). A dangling cached PC during teardown AVs into the
            // minimap_frame SEH guard, and the rebuild-reset clears the cache.
            UObject* pc = m_mm_pc;
            Engine::ParamsGetPawn gp{};
            if (!pc || !Engine::call(pc, L"K2_GetPawn", gp) || !gp.ReturnValue)
            {
                bool via_owning = false;
                pc = nullptr;
                if (m_minimap_canvas)
                {
                    Engine::ParamsGetOwningPlayer op{};
                    if (Engine::call(m_minimap_canvas, L"GetOwningPlayer", op) && op.ReturnValue)
                    {
                        pc = op.ReturnValue;
                        via_owning = true;
                    }
                }
                if (!pc)
                {
                    pc = player_controller();   // FindFirstOf fallback (a walk -- one-time on cache miss)
                }
                m_mm_pc = pc;
                if (pc && !m_mm_pc_logged)
                {
                    m_mm_pc_logged = true;
                    Output::send<LogLevel::Default>(STR("[Lodestone] minimap PC resolved via {}\n"),
                                                    via_owning ? STR("GetOwningPlayer") : STR("FindFirstOf"));
                }
                if (!pc || !Engine::call(pc, L"K2_GetPawn", gp) || !gp.ReturnValue)
                {
                    return false;
                }
            }
            if (!Engine::actor_location(gp.ReturnValue, px, py, pz))
            {
                return false;
            }
            // Heading = where the player is LOOKING (the controller's view yaw), NOT
            // where the pawn faces. In third-person the pawn lags the camera, so the
            // pawn's K2_GetActorRotation read as "the map doesn't respond to the camera"
            // (Kenny). GetControlRotation is on the controller, returns an FRotator like
            // K2_GetActorRotation, so the same param struct fits. If it ever fails the
            // arrow just holds (has_yaw stays false).
            Engine::ParamsGetActorRotation gr{};
            if (Engine::call(pc, L"GetControlRotation", gr))
            {
                yaw = gr.ReturnValue.Yaw;
                has_yaw = true;
            }
            else
            {
                // GetControlRotation can start failing after a world transition (the
                // cached controller went stale) -- which left the compass yaw-less and
                // dead mid-session. Fall back to the pawn's facing so the strip stays
                // alive (slightly laggy vs the free camera), and drop the PC cache so the
                // next frame re-resolves a fresh controller.
                Engine::ParamsGetActorRotation pr{};
                if (Engine::call(gp.ReturnValue, L"K2_GetActorRotation", pr))
                {
                    yaw = pr.ReturnValue.Yaw;
                    has_yaw = true;
                }
                m_mm_pc = nullptr;
            }
            return true;
        }

        // The compass is for walking, and you walk with the map CLOSED -- which is
        // precisely where tick() gives up and returns. So this runs above that
        // early-out, on its own clock, and is the one part of the mod that does work
        // while the map is not on screen.
        auto tick_compass() -> void
        {
            if (!g_ore_compass)
            {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < m_next_compass)
            {
                return;
            }
            double px = 0, py = 0, pz = 0;
            if (!player_pos(px, py, pz))
            {
                return;   // title or loading; leave the timer armed so we retry
            }
            m_next_compass = now + std::chrono::seconds(kCompassRefreshSec);
            update_ore_compass(px, py, pz);
        }

        // Which layer owns the rock at this spot?
        //
        // The compass's search results name no map object -- FPalCoopSkillSearchResultParameter
        // carries a Location and an InstanceId, not the DamagableRock id -- so the id
        // cannot be recovered from a hit. It does not need to be. A rock spawns exactly
        // on its spawner, which is the coordinate our tables already hold: the
        // calibration measured that agreement at 0.7u over 7/7 copper. So the position
        // IS the identity, and matching it live means no id->ore table to maintain, get
        // wrong, or re-derive when Pocketpair renumbers a rock.
        //
        // Returns -1 for a rock we have no layer for (pal crystal, plain stone), which
        // is the correct answer for those, not a failure.
        static auto layer_at(double x, double y) -> int
        {
            int best = -1;
            double best_d2 = kLayerMatchUU * kLayerMatchUU;
            for (size_t li = 0; li < std::size(Data::kLayers); ++li)
            {
                const auto& layer = Data::kLayers[li];
                for (size_t pi = 0; pi < layer.count; ++pi)
                {
                    const double dx = static_cast<double>(layer.points[pi].x) - x;
                    const double dy = static_cast<double>(layer.points[pi].y) - y;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < best_d2)
                    {
                        best_d2 = d2;
                        best = static_cast<int>(li);
                    }
                }
            }
            return best;
        }

        // What does the compass widget think it is pointing at?
        //
        // The icon draws with the right ART (vanilla's copper nugget) but reads 999m,
        // its 3-digit clamp, and sits nowhere near the real rock 90m west. So the id
        // resolves far enough to pick an icon, and the POSITION still comes out wrong.
        // Everything past that point is native and unreflected -- the dump cannot show
        // why -- so the only way to tell a bad Target Location from a bad LocationID
        // lookup is to read both off the widget and compare against the truth.
        //
        // Runs before the search, so it reports the state the LAST registration
        // produced rather than one we have not made yet.
        static constexpr const wchar_t* kVisNames[] = {L"Visible", L"Collapsed", L"Hidden", L"HitTestInvis",
                                                       L"SelfHitTestInvis"};


        // Where is this widget actually placed on its canvas?
        //
        // The last unmeasured thing. Every other property says our icons should draw:
        // right target, right distance, SelfHitTestInvisible, adopted by the same
        // IconCanvas the fast-travel icons use, and listed in VisibleIconIds. If the
        // slot offset is off-canvas or NaN, that closes it -- and comparing against a
        // fast-travel icon, which demonstrably draws, gives the number a meaning.
        static auto slot_desc(UObject* w) -> std::wstring
        {
            auto** slot = w ? w->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")) : nullptr;
            if (!slot || !*slot)
            {
                return L"<no slot>";
            }
            auto* ld = (*slot)->GetValuePtrByPropertyNameInChain<Engine::FAnchorData_>(STR("LayoutData"));
            if (!ld)
            {
                return L"<no LayoutData>";
            }
            auto* rt = w->GetValuePtrByPropertyNameInChain<Engine::FWidgetTransform_>(STR("RenderTransform"));
            wchar_t buf[200];
            std::swprintf(buf, std::size(buf), L"pos(%.0f,%.0f) anchor(%.2f,%.2f) render(%.0f,%.0f) scale(%.2f)",
                          ld->Offsets.Left, ld->Offsets.Top, ld->Anchors.MinX, ld->Anchors.MinY,
                          rt ? rt->TransX : -99999.0, rt ? rt->TransY : -99999.0, rt ? rt->ScaleX : -1.0);
            return buf;
        }

        auto probe_compass_icons(double px, double py, double pz) -> void
        {
            std::vector<UObject*> ws;
            UObjectGlobals::FindAllOf(STR("WBP_CompassIcon_ForMapObject_C"), ws);
            if (ws.empty())
            {
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] icon probe: 0 ForMapObject widgets (nothing registered, or the "
                        "compass built a different class)\n"));
                return;
            }
            Output::send<LogLevel::Default>(STR("[Lodestone] icon probe: {} widgets; player ({:.0f}, {:.0f})\n"),
                                            ws.size(), px, py);
            for (size_t i = 0; i < ws.size() && i < 10; ++i)
            {
                auto* tl = ws[i]->GetValuePtrByPropertyNameInChain<Engine::FVector_>(STR("Target Location"));
                auto* cd = ws[i]->GetValuePtrByPropertyNameInChain<double>(STR("CurrentDistance"));
                auto* hd = ws[i]->GetValuePtrByPropertyNameInChain<float>(STR("HiddenDistance"));
                auto* lid = ws[i]->GetValuePtrByPropertyNameInChain<Engine::FGuid_>(STR("MyLocationID"));
                const double real = tl ? std::sqrt((tl->X - px) * (tl->X - px) + (tl->Y - py) * (tl->Y - py))
                                       : -1.0;
                // "The widget exists with the right target" and "the widget draws" are
                // different claims, and the first one is now proven while the icons are
                // still not on screen. Visibility separates them; a parent of <none>
                // would say the compass built the widget but never adopted it, which
                // needs a different fix entirely.
                auto* vis = ws[i]->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
                Engine::ParamsGetParent gp{};
                const bool got_parent = Engine::call(ws[i], L"GetParent", gp);
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] icon probe:   [{}] target ({:.0f}, {:.0f}, {:.0f}) = {:.0f}u away | "
                        "CurrentDistance={:.0f} | Hidden={} | vis={} | slot {} | id "
                        "{:08X}-{:08X}-{:08X}-{:08X}\n"),
                    i, tl ? tl->X : 0.0, tl ? tl->Y : 0.0, tl ? tl->Z : 0.0, real, cd ? *cd : -1.0,
                    hd ? *hd : -1.0f, (vis && *vis <= 4) ? kVisNames[*vis] : STR("<unreadable>"),
                    slot_desc(ws[i]), lid ? lid->A : 0u, lid ? lid->B : 0u, lid ? lid->C : 0u,
                    lid ? lid->D : 0u);
                (void)got_parent;
                (void)gp;
            }
            // The differential that matters: fast-travel icons DEMONSTRABLY draw --
            // the P3a probe caught 3 of them SelfHitTestInvisible, which is what a
            // rendering compass icon looks like. Ours report the same visibility, the
            // same HiddenDistance and correct targets, and still do not appear. So the
            // difference is not in the widget; it is in WHERE the widget hangs.
            //
            // The census found TWO WBP_Ingame_Compass_C. If the register adopts our
            // icons into the other one -- an offscreen or inactive compass -- every
            // property we have read would look perfect and nothing would ever draw.
            // Comparing our parent's FULL name against a known-drawing icon's parent
            // is what tells those apart; GetName() says "IconCanvas" for both.
            // A reference that is DEFINITELY on screen.
            //
            // Comparing against a fast-travel icon proves nothing: it reports the same
            // visibility and slot as ours, but I cannot point at one on the screen, so
            // "identical to FT" is consistent with both of them being invisible. The
            // death mark IS visible -- the red X reading 378m -- and it derives from the
            // same WBP_CompassIconBase_C, so its RenderTransform is the number ours has
            // to look like.

            // Does the LOCATION MANAGER know our ids?
            //
            // The icons draw with the right art, at the right distance, and stacked at
            // the compass centre: render(0,0) on all seven, while the death mark -- the
            // red X I can see -- carries render(354,0). Seven rocks at seven bearings
            // cannot all be dead centre, so the offset is never computed for ours.
            //
            // The split that would explain it: the register's OnAddedLocationForCompass
            // delegate is what CREATES an icon, but the per-frame positioning walks
            // UPalLocationManager. If our ids never landed in the manager's LocationMap,
            // the compass would build a perfect icon it can never place -- which is
            // exactly what is on screen. GetLocationPoint is reflected, so just ask it.
            if (auto* lm = UObjectGlobals::FindFirstOf(STR("PalLocationManager")))
            {
                UFunction* fn = lm->GetFunctionByNameInChain(FName(STR("GetLocationPoint"), FNAME_Find));
                bool ok = fn != nullptr;
                if (fn)
                {
                    for (FProperty* pr : fn->ForEachProperty())
                    {
                        const auto nm = pr->GetName();
                        const int32_t off = pr->GetOffset_Internal();
                        if (nm == STR("ID") && off != 0x00) ok = false;
                        if (nm == STR("ReturnValue") && off != 0x10) ok = false;
                    }
                }
                if (!ok)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[Lodestone] icon probe:   GetLocationPoint layout unexpected, NOT calling\n"));
                }
                else if (auto* system2 = UObjectGlobals::FindFirstOf(STR("PalCoopSkillSearchSystem")))
                {
                    auto** lr2 = system2->GetValuePtrByPropertyNameInChain<UObject*>(STR("LocationRegister"));
                    auto* rids2 = (lr2 && *lr2) ? (*lr2)->GetValuePtrByPropertyNameInChain<Engine::FArrayHeader_>(
                                                      STR("RegisteredLocationIds"))
                                                : nullptr;
                    if (rids2 && rids2->Num > 0 && rids2->Data)
                    {
                        auto* g = static_cast<Engine::FGuid_*>(rids2->Data);
                        int known = 0;
                        for (int32_t k = 0; k < rids2->Num; ++k)
                        {
                            Engine::ParamsGetLocationPoint gl{};
                            gl.ID = g[k];
                            if (Engine::call(lm, L"GetLocationPoint", gl) && gl.ReturnValue)
                            {
                                ++known;
                            }
                        }
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] icon probe:   LocationManager knows {}/{} of our registered ids\n"),
                            known, rids2->Num);
                    }
                }
            }
            std::vector<UObject*> mo;
            UObjectGlobals::FindAllOf(STR("PalLocationPoint_MapObject"), mo);
            Output::send<LogLevel::Default>(STR("[Lodestone] icon probe:   PalLocationPoint_MapObject {}\n"),
                                            mo.size());

            std::vector<UObject*> deaths;
            UObjectGlobals::FindAllOf(STR("WBP_IngameCompass_DeathMark_C"), deaths);
            for (size_t i = 0; i < deaths.size() && i < 3; ++i)
            {
                auto* v = deaths[i]->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
                auto* cd = deaths[i]->GetValuePtrByPropertyNameInChain<double>(STR("CurrentDistance"));
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] icon probe:   DEATHMARK[{}] vis={} CurrentDistance={:.0f} slot {}\n"), i,
                    (v && *v <= 4) ? kVisNames[*v] : STR("?"), cd ? *cd : -1.0, slot_desc(deaths[i]));
            }
            std::vector<UObject*> fts;
            UObjectGlobals::FindAllOf(STR("WBP_IngameCompass_FastTravel_C"), fts);
            int shown = 0;
            for (auto* w : fts)
            {
                auto* v = w ? w->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility")) : nullptr;
                if (!v || *v == 1)
                {
                    continue;   // Collapsed: the compass hides these by arc, per Tick
                }
                Engine::ParamsGetParent fp{};
                Engine::call(w, L"GetParent", fp);
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] icon probe:   FT[{}] vis={} slot {} parent={}\n"), shown,
                    (*v <= 4) ? kVisNames[*v] : STR("?"), slot_desc(w),
                    fp.ReturnValue ? fp.ReturnValue->GetFullName() : STR("<none>"));
                if (++shown >= 3)
                {
                    break;
                }
            }
            // Created and SHOWN are two different lists.
            //
            // The compass keeps CreatedIconMap (every icon it built) and a separate
            // VisibleIconIds (the ids it will actually draw), refreshed by
            // GetVisibleIcons() off UpdateVisibleTimer -- not per tick. Our 7 widgets
            // prove we made it into the first list. If our ids are missing from the
            // second, the widget can be perfect forever and never appear, which is
            // exactly what we are looking at.
            //
            // The ids to compare against are the register's own RegisteredLocationIds:
            // whatever the register minted for our rocks is what the compass would have
            // to list here.
            for (auto* c : compasses_live())
            {
                auto* vids =
                    c->GetValuePtrByPropertyNameInChain<Engine::FArrayHeader_>(STR("VisibleIconIds"));
                auto* v = c->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] icon probe:   compass vis={} VisibleIconIds={} {}\n"),
                    (v && *v <= 4) ? kVisNames[*v] : STR("?"), vids ? vids->Num : -1, c->GetFullName());
                if (vids && vids->Num > 0 && vids->Data)
                {
                    auto* g = static_cast<Engine::FGuid_*>(vids->Data);
                    for (int32_t k = 0; k < vids->Num && k < 12; ++k)
                    {
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] icon probe:     visible id {:08X}-{:08X}-{:08X}-{:08X}\n"), g[k].A,
                            g[k].B, g[k].C, g[k].D);
                    }
                }
            }
            if (auto* system = UObjectGlobals::FindFirstOf(STR("PalCoopSkillSearchSystem")))
            {
                auto** lr = system->GetValuePtrByPropertyNameInChain<UObject*>(STR("LocationRegister"));
                if (lr && *lr)
                {
                    auto* rids =
                        (*lr)->GetValuePtrByPropertyNameInChain<Engine::FArrayHeader_>(
                            STR("RegisteredLocationIds"));
                    Output::send<LogLevel::Default>(STR("[Lodestone] icon probe:   RegisteredLocationIds={}\n"),
                                                    rids ? rids->Num : -1);
                    if (rids && rids->Num > 0 && rids->Data)
                    {
                        auto* g = static_cast<Engine::FGuid_*>(rids->Data);
                        for (int32_t k = 0; k < rids->Num && k < 12; ++k)
                        {
                            Output::send<LogLevel::Default>(
                                STR("[Lodestone] icon probe:     registered id {:08X}-{:08X}-{:08X}-{:08X}\n"),
                                g[k].A, g[k].B, g[k].C, g[k].D);
                        }
                    }
                }
            }
        }

        // The LIVE compass only. FindAllOf also hands back the blueprint's own
        // template (/Game/Pal/Blueprint/UI/WBP_PlayerUI.WBP_PlayerUI_C:WidgetTree...),
        // which is not a widget on anyone's screen -- counting it is what made the P3a
        // census report "2 instances" and sent this chase after a second compass that
        // does not exist. A live UMG widget lives under /Engine/Transient.
        static auto compasses_live() -> std::vector<UObject*>
        {
            std::vector<UObject*> all, live;
            UObjectGlobals::FindAllOf(STR("WBP_Ingame_Compass_C"), all);
            for (auto* c : all)
            {
                if (c && c->GetFullName().find(STR("/Engine/Transient")) != std::wstring::npos)
                {
                    live.push_back(c);
                }
            }
            return live;
        }

        // Put the ore you asked for on vanilla's own compass.
        //
        // Vanilla builds this for the Pal search skills: register a found map object and
        // it draws with that rock's own icon, at the right bearing, with a distance --
        // no widget of ours, native by construction. We drive the same pipeline:
        //   System::CreateSearchObject(PalCoopSkillSearchMapObject)
        //   SearchMapObjects(ids, origin, radius, uid)  -> real hits, real InstanceIds
        //   Register::RegisterMapObjectLocationsToCompass(hit) x N
        // Forging a hit does not work -- the register drops a parameter whose InstanceId
        // names no live map object, which is why this runs the real search.
        //
        // Scope, measured rather than assumed: SearchMapObjects only ever returns rocks
        // that are STREAMED IN. A 10km radius from a base returned 7 copper and nothing
        // else, because nothing else was loaded. That is not a limitation worth fighting:
        // the compass widgets hide past HiddenDistance=30000 (300m) anyway, and the
        // streamed region is far larger than that, so every rock the compass could draw
        // is a rock the search can see.
        auto update_ore_compass(double px, double py, double pz) -> void
        {
            auto* system = UObjectGlobals::FindFirstOf(STR("PalCoopSkillSearchSystem"));
            if (!system)
            {
                return;
            }
            auto** lr = system->GetValuePtrByPropertyNameInChain<UObject*>(STR("LocationRegister"));
            UObject* reg = (lr && *lr) ? *lr : nullptr;
            if (!reg)
            {
                return;
            }
            auto* cls = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Script/Pal.PalCoopSkillSearchMapObject"));
            if (!cls)
            {
                return;
            }
            Engine::FGuid_ uid{};
            if (auto* pc = UObjectGlobals::FindFirstOf(STR("PalPlayerController")))
            {
                Engine::ParamsGetPlayerUId gu{};
                if (Engine::call(pc, L"GetPlayerUId", gu))
                {
                    uid = gu.ReturnValue;
                }
            }
            if (!m_probed_icons)
            {
                m_probed_icons = true;   // once per session: enough to diagnose, no 45s spam
                probe_compass_icons(px, py, pz);
            }
            Engine::ParamsCreateSearchObject cso{};
            cso.SearchClass = cls;
            if (!Engine::call(system, L"CreateSearchObject", cso) || !cso.ReturnValue)
            {
                return;
            }
            if (!search_params_ok(cso.ReturnValue))
            {
                return;
            }
            // Start the searcher the way vanilla does, before searching by hand.
            //
            // Seven theories are now dead and every ingredient checks out, which points
            // at the one thing we never did: we register behind the system's back. The
            // system reports IsRunning=false the whole time -- we call
            // CreateSearchObject and then drive SearchMapObjects ourselves, so as far as
            // the game is concerned no search is happening. If the compass only places
            // search icons while a search is live, that gates placement while leaving
            // every field we have probed perfectly correct, which is exactly the shape
            // of what we see.
            //
            // Start() is the base's real entry point (Pal.hpp: SearchOrigin, SkillRank,
            // bIsRunning, and Tick(dt, register) is what registers natively).
            {
                UFunction* sfn = cso.ReturnValue->GetFunctionByNameInChain(FName(STR("Start"), FNAME_Find));
                bool sok = sfn != nullptr;
                if (sfn)
                {
                    for (FProperty* pr : sfn->ForEachProperty())
                    {
                        const auto nm = pr->GetName();
                        const int32_t off = pr->GetOffset_Internal();
                        if (nm == STR("Origin") && off != 0x00) sok = false;
                        if (nm == STR("Rank") && off != 0x18) sok = false;
                        if (nm == STR("RequestPlayerUId") && off != 0x1C) sok = false;
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] compass: Start param {:<20} @ {:#04x}\n"), nm, off);
                    }
                }
                if (sok)
                {
                    Engine::ParamsStart sp2{};
                    sp2.Origin = {px, py, pz};
                    sp2.Rank = 1;
                    sp2.RequestPlayerUId = uid;
                    Engine::call(cso.ReturnValue, L"Start", sp2);
                    auto* running = cso.ReturnValue->GetValuePtrByPropertyNameInChain<bool>(STR("bIsRunning"));
                    Engine::ParamsIsRunning sysrun{};
                    const bool got = Engine::call(system, L"IsRunning", sysrun);
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] compass: Start() -> searcher bIsRunning={} system IsRunning={}\n"),
                        running ? (*running ? STR("true") : STR("false")) : STR("?"),
                        got ? (sysrun.ReturnValue ? STR("true") : STR("false")) : STR("<no fn>"));
                }
                else
                {
                    Output::send<LogLevel::Warning>(
                        STR("[Lodestone] compass: Start layout unexpected, NOT calling\n"));
                }
            }
            // Every ore id DT_LocationUIData knows an icon for. Searching all six in one
            // call rather than one call per enabled layer: the game's cost is a walk over
            // the loaded map objects either way, and the toggle filter happens below on
            // the results, where it is exact.
            static FName ore_ids[] = {
                FName(STR("DamagableRock0001"), FNAME_Add), FName(STR("DamagableRock0002"), FNAME_Add),
                FName(STR("DamagableRock0003"), FNAME_Add), FName(STR("DamagableRock0004"), FNAME_Add),
                FName(STR("DamagableRock0005"), FNAME_Add), FName(STR("DamagableRock0006"), FNAME_Add)};
            Engine::ParamsSearchMapObjects sp{};
            sp.SearchMapObjIds.Data = ore_ids;
            sp.SearchMapObjIds.Num = static_cast<int32_t>(std::size(ore_ids));
            sp.SearchMapObjIds.Max = sp.SearchMapObjIds.Num;
            sp.Origin = {px, py, pz};
            sp.SearchRadius = static_cast<float>(kCompassRangeUU);
            sp.RequestPlayerUId = uid;
            Engine::call(cso.ReturnValue, L"SearchMapObjects", sp);

            auto* res = cso.ReturnValue->GetValuePtrByPropertyNameInChain<Engine::FArrayHeader_>(
                STR("SearchResultParameters"));
            if (!res || res->Num <= 0 || !res->Data)
            {
                return;
            }
            auto* found = static_cast<Engine::FPalCoopSkillSearchResultParameter_*>(res->Data);
            // Keep the ones whose layer is switched on, nearest first.
            std::vector<std::pair<double, int32_t>> keep;
            for (int32_t i = 0; i < res->Num; ++i)
            {
                const int layer = layer_at(found[i].Location.X, found[i].Location.Y);
                if (layer < 0 || !is_layer_on(layer))
                {
                    continue;
                }
                const double dx = found[i].Location.X - px;
                const double dy = found[i].Location.Y - py;
                keep.emplace_back(std::sqrt(dx * dx + dy * dy), i);
            }
            std::sort(keep.begin(), keep.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            // A cap, because clustering makes radius alone useless as a bound: one seam
            // of coal is dozens of rocks inside 50m, and a compass showing all of them is
            // a smear that hides everything else. Nearest-N is what a navigator wants.
            if (keep.size() > kCompassMaxIcons)
            {
                keep.resize(kCompassMaxIcons);
            }
            // Read the register's own array rather than counting our calls. "7
            // registered" previously meant "we called it 7 times", which cannot tell
            // an accepted parameter from a rejected one -- and rejection is exactly
            // what happened to the forged parameters this pipeline replaced.
            auto* reg_ids = reg->GetValuePtrByPropertyNameInChain<Engine::FArrayHeader_>(
                STR("RegisteredLocationIds"));
            const int32_t before = reg_ids ? reg_ids->Num : -1;
            for (const auto& [dist, idx] : keep)
            {
                Engine::ParamsRegisterToCompass rp{};
                rp.ResultParameter = found[idx];
                Engine::call(reg, L"RegisterMapObjectLocationsToCompass", rp);
            }
            const int32_t after = reg_ids ? reg_ids->Num : -1;
            // Nudge the compass to place what we just registered.
            //
            // Every ingredient is verified present: the manager knows 7/7 of our ids,
            // 7 PalLocationPoint_MapObject exist, each widget's MyLocationID matches a
            // registered id, targets and CurrentDistance are right. The ONLY thing
            // missing is the RenderTransform -- ours read (0,0) while the death mark,
            // which is on screen, reads (354,0). The icon's own Tick clearly runs,
            // since it is what computes CurrentDistance, so placement is the compass's
            // job, not the icon's.
            //
            // 'Update Icon' is a reflected, no-argument BP function on the compass.
            // The guess worth one call: it is event-driven off vanilla's own search
            // starting, and registering behind its back never triggers it. If that is
            // right, the icons snap to their bearings; if the translations stay 0, the
            // placement is somewhere else entirely and no more poking will find it.
            // (The space in the name is real -- BP function names keep them.)
            for (auto* c : compasses_live())
            {
                struct
                {
                } noargs;
                Engine::call(c, L"Update Icon", noargs);
            }
            // Only on a change: this runs every 45s forever, and a line per refresh
            // would bury every other diagnostic in the log within an hour.
            if (keep.size() != m_last_compass_count || res->Num != m_last_compass_hits)
            {
                m_last_compass_count = keep.size();
                m_last_compass_hits = res->Num;
                const double nearest_m = keep.empty() ? -1.0 : keep.front().first / 100.0;
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] compass: {} rocks streamed, {} on enabled layers, sent {}; "
                        "RegisteredLocationIds {} -> {}; nearest {:.0f}m\n"),
                    res->Num, keep.size(), keep.size(), before, after, nearest_m);
            }
        }


        auto place_dots() -> void
        {
            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            if (!image_class || !m_calibration)
            {
                return;
            }
            m_emit_cursor = 0;
            m_layer_ranges.clear();
            m_ranges_ok = true;
            size_t placed = 0;
            const auto t0 = std::chrono::steady_clock::now();
            int layer_index = 0;
            for (const auto& layer : Data::kLayers)
            {
                const int this_layer = layer_index++;
                // Create dots ONLY for layers that are on. Creating all ~12k dots for
                // every layer (the ore fields are thousands each) balloons the widget
                // count in one canvas, and on a server the map body is rebuilt constantly
                // (flying = streaming), re-creating the whole pool until Slate crashes --
                // even with nothing toggled (Kenny). Bounding the pool to the on-set (a
                // handful of layers, ~1-2k dots) is the fix. Toggling a currently-off
                // layer ON has no dots to show, so poll_panel forces a re-place then.
                if (!is_layer_on(this_layer))
                {
                    continue;
                }
                const Engine::FLinearColor_ color{Style::srgb8(layer.r), Style::srgb8(layer.g),
                                                  Style::srgb8(layer.b), layer.a / 255.0f};
                for (size_t i = 0; i < layer.count; ++i)
                {
                    const auto pos = m_calibration->transform.apply(layer.points[i].x, layer.points[i].y);
                    if (pos.x != pos.x || pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
                    {
                        continue;
                    }
                    if (emit_dot(image_class, pos,
                                 {static_cast<double>(layer.points[i].x), static_cast<double>(layer.points[i].y)},
                                 color, layer.icon, kLayerDotPx, true, this_layer) != SIZE_MAX)
                    {
                        ++placed;
                    }
                }
            }
            // effigies & notes, filtered by the live collected set
            std::unordered_set<std::wstring> collected;
            const bool have_flags = Collected::gather(collected);
            // one-shot diagnostic: reveals the obtain-flag key format vs our GUIDs
            if (!m_flag_probe_done)
            {
                m_flag_probe_done = true;
                std::wstring samples;
                int n = 0;
                for (const auto& k : collected)
                {
                    samples += k;
                    samples += L' ';
                    if (++n >= 4)
                    {
                        break;
                    }
                }
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] gathered {} obtained keys; samples: {}\n"), collected.size(),
                    samples);
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] our eff[0]={} note[0]={}\n"),
                    Collected::guid_key(Data::kEffigies[0].guid), std::wstring(Data::kNotes[0].row));
            }
            size_t hidden = 0;
            // Generic collectable placement: key_fn(i) yields the obtained-set key
            // (instance GUID for effigies, NoteRowName for notes).
            // backing != nullptr draws a coloured disc UNDER the icon (a "map pin" halo
            // so item art pops on any biome) -- both the backing and the icon share the
            // collect key so they hide together. backing_size is the disc diameter.
            auto place_collectables = [&](size_t count, auto coord_fn, auto key_fn,
                                          const Engine::FLinearColor_& color, const wchar_t* icon,
                                          double base_size, int layer_id,
                                          const std::function<bool(size_t)>& filter_fn = {},
                                          const std::function<const wchar_t*(size_t)>& icon_fn = {},
                                          const Engine::FLinearColor_* backing = nullptr,
                                          double backing_size = 0.0, double backing_border = 0.0,
                                          const Engine::FLinearColor_* backing_border_col = nullptr) -> size_t {
                size_t layer_hidden = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    if (filter_fn && !filter_fn(i))
                    {
                        continue;   // filtered out (e.g. effigy of a non-selected type)
                    }
                    std::wstring key = key_fn(i);
                    const bool is_collected = have_flags && collected.contains(key);
                    if (is_collected)
                    {
                        ++hidden;
                        ++layer_hidden;
                    }
                    const auto xy = coord_fn(i);
                    const auto pos = m_calibration->transform.apply(xy.first, xy.second);
                    if (pos.x < -2000 || pos.x > 6000 || pos.y < -2000 || pos.y > 6000)
                    {
                        continue;
                    }
                    const Project::Vec2 world{static_cast<double>(xy.first), static_cast<double>(xy.second)};
                    // Backing disc first so it sits UNDER the icon (z = emit order).
                    if (backing)
                    {
                        const size_t bidx = emit_dot(image_class, pos, world, *backing, nullptr,
                                                     backing_size, !is_collected, layer_id, backing_border,
                                                     backing_border_col);
                        if (bidx != SIZE_MAX)
                        {
                            m_guid_dots.push_back({bidx, key});   // copy: key reused below
                            ++placed;
                        }
                    }
                    // coord_fn yields std::pair<int,int>; the casts are required, not
                    // stylistic -- int -> double inside braces is a narrowing
                    // conversion unless the source is a constant expression.
                    // Per-effigy icon (each relic type is a distinct Pal statue with its own
                    // in-game icon); falls back to the fixed layer icon when no override.
                    const wchar_t* dot_icon = icon_fn ? icon_fn(i) : icon;
                    const size_t idx = emit_dot(image_class, pos, world, color, dot_icon, base_size,
                                                !is_collected, layer_id);
                    if (idx != SIZE_MAX)
                    {
                        m_guid_dots.push_back({idx, std::move(key)});
                        ++placed;
                    }
                }
                return layer_hidden;
            };
            // When a relic type is picked (g_track_relic >= 0), show only that type's
            // effigies (the compass then guides to the nearest of it). -1 = all types.
            const size_t eff_hidden = place_collectables(
                std::size(Data::kEffigies),
                [](size_t i) { return std::pair<int, int>{Data::kEffigies[i].x, Data::kEffigies[i].y}; },
                [](size_t i) { return Collected::guid_key(Data::kEffigies[i].guid); },
                {0.35f, 1.0f, 0.20f, 1.0f}, Data::kEffigyIcon, kCollectDotPx, kEffigyLayer,
                [](size_t i) {
                    return g_track_relic < 0 ||
                           (i < Data::kEffigyTypeCount && Data::kEffigyType[i] == g_track_relic);
                },
                [](size_t i) {
                    // Each effigy shows its own relic-type icon (Lifmunk/Lamball/Pengullet/...).
                    const int t = (i < Data::kEffigyTypeCount) ? Data::kEffigyType[i] : 0;
                    return (t >= 0 && t < Data::kRelicTypeCount) ? Data::kRelicTypeIcon[t]
                                                                 : Data::kEffigyIcon;
                });
            // Notes: the technology-book icon on an AMBER map pin with a dark ring border
            // (was bright magenta -- "way too pink", per Kenny). The disc makes the book
            // pop on any biome (item art can't be tinted); amber is warm, on-brand
            // (tehAon house colour) and not used by another default-on layer, and the ring
            // gives it a crisp coin edge instead of a flat blob.
            static constexpr Engine::FLinearColor_ kNoteBacking{0.94f, 0.66f, 0.24f, 1.0f};
            static constexpr Engine::FLinearColor_ kNoteBorder{0.10f, 0.08f, 0.05f, 1.0f};
            const size_t note_hidden = place_collectables(
                std::size(Data::kNotes),
                [](size_t i) { return std::pair<int, int>{Data::kNotes[i].x, Data::kNotes[i].y}; },
                [](size_t i) { return std::wstring(Data::kNotes[i].row); },
                {1.0f, 1.0f, 1.0f, 1.0f}, Data::kNoteIcon, 16.0, kNoteLayer, {}, {}, &kNoteBacking, 24.0,
                2.5, &kNoteBorder);
            Output::send<LogLevel::Default>(
                STR("[Lodestone] hidden effigies={}/{} notes={}/{}\n"), eff_hidden,
                std::size(Data::kEffigies), note_hidden, std::size(Data::kNotes));
            recompute_found(collected, have_flags);   // seed the found/total badges
            // Everything above is stable for the session (static layers + collectables);
            // everything BELOW is a live actor scan that goes stale as the world changes
            // (eggs get picked up, towers get unlocked). Mark the boundary so a map reopen
            // re-runs only this tail via refresh_live_layers, not the whole dense pool.
            m_live_begin = m_emit_cursor;
            // live eggs: no static positions (random lottery placement + respawn),
            // so enumerate the PalEgg loot actors currently loaded around the player
            // and place a dot at each. Only covers the streamed-in area near you; the
            // set refreshes whenever the map is reopened from a new location.
            const size_t egg_shown = place_live_eggs(image_class);
            placed += egg_shown;
            Output::send<LogLevel::Default>(STR("[Lodestone] live eggs placed={}\n"), egg_shown);
            placed += place_live_pois(image_class);
            placed += place_tracked_pal(image_class);   // track-one-Pal spawn points
            placed += place_nearest_dungeons(image_class);   // nearest cave entrances (Build 1.7)
            placed += place_nearest_lucky(image_class);   // nearest lucky/rare wild Pal (Build B)
            // collapse any leftover pooled dots beyond this pass
            for (size_t i = m_emit_cursor; i < m_dots.size(); ++i)
            {
                Engine::ParamsSetVisibility vis{Engine::Vis_Collapsed};
                Engine::call(m_dots[i].widget, L"SetVisibility", vis);
            }
            // Paint every VISIBLE dot before this pass returns, rather than leaving
            // them to the per-tick batch. The batch is why the map used to open as
            // colour squares: at 400/tick a 7.6k pool takes ~19 ticks to cover, and
            // most of that budget went on dots that were toggled off anyway.
            //
            // Doing it inline is affordable here precisely because placement already
            // costs ~140ms -- this is one bounded sweep over the pool on a frame that
            // is already hitching, not a new cost on a smooth one. Only the ~1.1k
            // visible dots are touched; the other ~6.5k stay with the background pass.
            //
            // m_icons_ready is the acquire that publishes m_tex_index from the game
            // thread; without it this pass would race the loader. If icons are not
            // ready yet the batch picks them up later, as it always did.
            const size_t pre_painted =
                (g_icons_enabled && m_icons_ready.load(std::memory_order_acquire))
                    ? paint_pass(SIZE_MAX, true, m_icon_scan_vis)
                    : 0;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            Output::send<LogLevel::Default>(
                STR("[Lodestone] {} dots placed, {} collected hidden (pool {}), {} icons painted inline in {}ms\n"),
                placed, hidden, m_dots.size(), pre_painted, ms);
            if (!m_logged_census)
            {
                m_logged_census = true;
                census_locations();
            }
            m_placed = true;
            m_collapsed = false;

            // Leak check for the doubled-overlay report. Our overlay is a CanvasPanel named
            // Lodestone_Overlay*, so find those DIRECTLY -- the old walk descended each
            // WBP_Map_Body_C via GetChildAt, but a map body is a UUserWidget (no panel
            // children), so it always read 0 and could neither confirm nor deny the leak.
            // Total stays small if ensure_layer_canvas keeps sweeping reused bodies; an
            // unbounded climb across opens is the leak (two overlays paint -> "2 where 1").
            {
                std::vector<UObject*> canvases;
                UObjectGlobals::FindAllOf(STR("CanvasPanel"), canvases);
                int overlays = 0;
                for (auto* c : canvases)
                {
                    if (c && Engine::widget_name(c).starts_with(L"Lodestone_Overlay"))
                    {
                        ++overlays;
                    }
                }
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] Lodestone_Overlay canvases live: {}\n"), overlays);
            }
        }

        // Re-scan ONLY the live layers on a map reopen. Static + collectable dots (below
        // m_live_begin) are stable for the session, so rewind the emit cursor to there and
        // re-run just the live scans: a collected egg (its actor despawned) drops out, a
        // tower unlocked since the last open is skipped by place_live_pois' IsUnlockMapPoint
        // filter, and the compass follows for free (compute_nearest reads the same pool).
        // Cheap -- the live tail is a few hundred sparse dots, so this avoids both the
        // ~400ms full-replace hitch and the dense-reveal Slate spike. Re-emitted dots are
        // created Collapsed; the caller's refresh_collected -> apply_layer_visibility queues
        // them through drain_reveals like every other reopen reveal.
        auto refresh_live_layers() -> void
        {
            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            if (!image_class || !m_calibration || !m_placed)
            {
                return;
            }
            m_emit_cursor = m_live_begin;
            // Erase the live-layer ranges first so emit_dot rebuilds them contiguously from
            // m_live_begin -- re-emitting into a still-present range trips emit_dot's
            // "layer resumed non-contiguously" guard and disables per-layer ranges pool-wide.
            for (int lid : {kEggLayer, kFastTravelLayer, kTowerLayer, kSealedLayer, kPalSpeciesLayer,
                            kDungeonLayer, kLuckyLayer})
            {
                m_layer_ranges.erase(lid);
            }
            size_t placed = 0;
            placed += place_live_eggs(image_class);
            placed += place_live_pois(image_class);
            placed += place_tracked_pal(image_class);
            placed += place_nearest_dungeons(image_class);
            placed += place_nearest_lucky(image_class);
            // A shorter tail than last open (an egg picked up, a tower unlocked) leaves stale
            // dots beyond the new cursor -- collapse them. Everything below m_live_begin (<=
            // m_emit_cursor) is static/collectable and is never touched.
            for (size_t i = m_emit_cursor; i < m_dots.size(); ++i)
            {
                if (m_dots[i].slot)
                {
                    Engine::ParamsSetVisibility vis{Engine::Vis_Collapsed};
                    Engine::call(m_dots[i].widget, L"SetVisibility", vis);
                }
            }
            Output::send<LogLevel::Default>(
                STR("[Lodestone] live layers re-scanned on reopen: {} placed (tail {}..{})\n"), placed,
                m_live_begin, m_emit_cursor);
        }

        // Visibility = layer toggled on AND not collected. Cheap per-tick-safe
        // diff (only SetVisibility, never re-parent).
        // stage_hide budgets the OFF direction too. A whole dense layer going visible in one
        // frame spiked Slate and crashed the game (RedBerry 1939) -- so a show is always
        // queued. A bulk HIDE hits the SAME cliff: toggling Junk (2757) + Chest (700) off
        // together collapsed ~3400 children in one frame and crashed. So on a real toggle
        // (apply_layers) a hide is queued too. The first-open / reopen sweep
        // (apply_layer_visibility) leaves stage_hide false because every off-layer dot is
        // already collapsed there (created collapsed; on-layers collapsed by
        // collapse_shown_dots before reopen), so an immediate collapse is a no-op, never a
        // bulk relayout -- and keeping shows-only there preserves the fast map-open populate.
        auto apply_dot(size_t idx, bool stage_hide = false) -> bool
        {
            const Dot& d = m_dots[idx];
            const bool show = is_layer_on(d.layer_id) && !d.base_hidden;
            if (show || stage_hide)
            {
                // drain_reveals recomputes desired state per dot at drain time, so a
                // toggle-off before a queued reveal drains just collapses (and vice versa).
                m_reveal_queue.push_back(idx);
            }
            else
            {
                Engine::ParamsSetVisibility vis{Engine::Vis_Collapsed};
                Engine::call(d.widget, L"SetVisibility", vis);
            }
            return show;
        }

        // Apply queued visibility flips a bounded number per frame. See apply_dot for why:
        // changing a whole dense layer's visibility at once spikes Slate and crashed the
        // game -- both on reveal and on bulk hide. Recomputes the desired state per dot at
        // drain time, so the queue self-corrects: a dot toggled on then off before it drains
        // simply collapses. Every drained dot spends budget (a hide is as Slate-heavy as a
        // show), so the per-frame cap bounds churn in BOTH directions.
        auto drain_reveals() -> void
        {
            if (m_reveal_queue.empty())
            {
                return;
            }
            size_t done = 0, i = 0;
            for (; i < m_reveal_queue.size() && done < kRevealBudget; ++i)
            {
                const size_t idx = m_reveal_queue[i];
                if (idx >= m_dots.size())
                {
                    continue;
                }
                const Dot& d = m_dots[idx];
                if (!d.slot)
                {
                    continue;
                }
                const bool show = is_layer_on(d.layer_id) && !d.base_hidden;
                Engine::ParamsSetVisibility vis{show ? Engine::Vis_HitTestInvisible
                                                     : Engine::Vis_Collapsed};
                Engine::call(d.widget, L"SetVisibility", vis);
                ++done;
            }
            m_reveal_queue.erase(m_reveal_queue.begin(), m_reveal_queue.begin() + i);
        }

        // Collapse the dots that are currently shown (the on-layers), bounded by the
        // on-set via m_layer_ranges rather than the whole pool. Used before re-showing
        // the overlay canvas on a map reopen: the dots stay individually visible across a
        // close (only the parent canvas is collapsed), so re-showing the parent would
        // otherwise reveal a dense on-layer all at once -- the RedBerry Slate spike, but
        // on reopen instead of on toggle. Collapsing them first lets the reopen re-stage
        // the reveal through the queue. Falls back to the whole pool if ranges are off.
        auto collapse_shown_dots() -> void
        {
            auto collapse = [](const Dot& d) {
                Engine::ParamsSetVisibility v{Engine::Vis_Collapsed};
                Engine::call(d.widget, L"SetVisibility", v);
            };
            if (!m_ranges_ok)
            {
                for (const auto& d : m_dots)
                {
                    if (d.slot)
                    {
                        collapse(d);
                    }
                }
                return;
            }
            for (const auto& [id, range] : m_layer_ranges)
            {
                if (!is_layer_on(id))
                {
                    continue;
                }
                const size_t end = std::min(range.second, m_dots.size());
                for (size_t i = range.first; i < end; ++i)
                {
                    if (m_dots[i].slot)
                    {
                        collapse(m_dots[i]);
                    }
                }
            }
        }

        // Re-apply only the layers whose checkbox actually moved.
        //
        // A click used to cost a SetVisibility on every dot in the pool -- ~7.6k
        // ProcessEvent calls to change one layer, and the parity layers roughly double
        // that. Each layer owns a contiguous run, so touch that and nothing else.
        // Falls back to the full sweep if the emit order ever stops being contiguous
        // (see m_layer_ranges) or if a layer somehow has no range.
        auto apply_layers(const std::vector<int>& layer_ids) -> void
        {
            if (!m_ranges_ok)
            {
                apply_layer_visibility();
                return;
            }
            size_t touched = 0;
            for (int id : layer_ids)
            {
                auto it = m_layer_ranges.find(id);
                if (it == m_layer_ranges.end())
                {
                    continue;   // layer has no dots placed (e.g. no live POIs this open)
                }
                const size_t end = std::min(it->second.second, m_dots.size());
                for (size_t i = it->second.first; i < end; ++i)
                {
                    if (m_dots[i].slot)
                    {
                        apply_dot(i, /*stage_hide=*/true);   // budget the OFF churn too
                        ++touched;
                    }
                }
            }
            log_visible_layers(touched, true);
        }

        auto apply_layer_visibility() -> void
        {
            size_t shown = 0;
            for (size_t i = 0; i < m_dots.size(); ++i)
            {
                if (!m_dots[i].slot)
                {
                    continue;
                }
                shown += apply_dot(i) ? 1 : 0;
            }
            log_visible_layers(shown, false);
        }

        // "dots placed" counts every dot CREATED (all layers, on or off), so it says
        // nothing about what you actually see. This does. If it disagrees with the
        // panel's checkboxes, layer state and widget state have drifted.
        //
        // Shared by both paths on purpose: a fast path that reported differently from
        // the slow one would hide exactly the drift this line exists to catch. `partial`
        // says whether the count is the whole pool or just the layers re-applied, so a
        // range-path number is never mistaken for a pool-wide total.
        //
        // Logged on change rather than on a budget. A budget runs out exactly when a bug
        // shows up hours into a session -- that already cost one round-trip, where the
        // last six lines predated the report and the interesting toggles logged nothing
        // at all. On-change is self rate-limiting and always shows the current truth.
        auto log_visible_layers(size_t shown, bool partial) -> void
        {
            std::wstring on;
            for (int i = 0; i < static_cast<int>(std::size(Data::kLayers)); ++i)
            {
                if (is_layer_on(i))
                {
                    on += Data::kLayers[i].key;
                    on += L' ';
                }
            }
            // the collectable and live-POI layers are not in kLayers, so name them
            // too or toggling them looks like nothing happened
            for (const auto& e : kExtraLayers)
            {
                if (is_layer_on(e.id))
                {
                    on += e.key;
                    on += L' ';
                }
            }
            if (on != m_last_vis_log)
            {
                m_last_vis_log = on;
                if (partial)
                {
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] re-applied {} dots in the changed layer(s) (pool {}); layers on: {}\n"),
                        shown, m_dots.size(), on.empty() ? std::wstring(L"(none)") : on);
                }
                else
                {
                    Output::send<LogLevel::Default>(STR("[Lodestone] visible {}/{} dots; layers on: {}\n"),
                                                    shown, m_dots.size(),
                                                    on.empty() ? std::wstring(L"(none)") : on);
                }
            }
        }

        // ⚠ never re-parent pooled widgets on refresh: 5k AddChild churn per
        // open crashed both the Lua prototype and the first P1.5 build.
        auto refresh_collected() -> void
        {
            if (m_guid_dots.empty())
            {
                return;
            }
            std::unordered_set<std::wstring> collected;
            if (!Collected::gather(collected))
            {
                return;
            }
            size_t hidden = 0;
            std::unordered_set<std::wstring> dot_keys;
            dot_keys.reserve(m_guid_dots.size());
            for (const auto& gd : m_guid_dots)
            {
                const bool is_collected = collected.contains(gd.key);
                hidden += is_collected ? 1 : 0;
                m_dots[gd.dot_index].base_hidden = is_collected;
                dot_keys.insert(gd.key);
            }
            recompute_found(collected, true);   // keep the found/total badges current
            apply_layer_visibility();
            // Diagnose "collected effigy still shows": an obtained key that matches NO
            // dot means we hold a wrong/stale GUID for that effigy (or note) -- the
            // copper-style version drift. A 32-hex key is an effigy GUID; anything else
            // (e.g. "Day11") is a note name.
            auto is_hex32 = [](const std::wstring& s) {
                if (s.size() != 32)
                {
                    return false;
                }
                for (wchar_t c : s)
                {
                    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
                    {
                        return false;
                    }
                }
                return true;
            };
            size_t orphan_effigy = 0, orphan_note = 0;
            for (const auto& k : collected)
            {
                if (dot_keys.contains(k))
                {
                    continue;
                }
                (is_hex32(k) ? orphan_effigy : orphan_note)++;
            }
            Output::send<LogLevel::Default>(
                STR("[Lodestone] collected refresh: {} hidden; {} obtained-effigy + {} obtained-note keys "
                    "match no dot (stale GUIDs?)\n"),
                hidden, orphan_effigy, orphan_note);
        }

        // Paint at most `budget` pending icon textures per call (non-blocking).
        // Runs across ticks so placement stays instant and icons pop in smoothly;
        // painting all of them inline froze the game for upstream.
        //
        // Loading happens on the game thread (see tick_load_icons); this only
        // reads the finished index, and only once m_icons_ready says the writes
        // are published.
        size_t m_icon_scan = 0;       // background pass: every dot, painted or not
        size_t m_icon_scan_vis = 0;   // priority pass: only what is on screen

        // Swap one dot's colour square for its icon. Returns false if it is already
        // painted, has no icon, or the texture is not loaded.
        auto paint_one(Dot& d) -> bool
        {
            if (!d.icon || d.icon_applied || !d.widget)
            {
                return false;
            }
            auto* tex = layer_texture(d.icon);
            if (!tex)
            {
                return false;
            }
            Engine::ParamsSetBrushFromTexture brush{tex, false};
            Engine::call(d.widget, L"SetBrushFromTexture", brush);
            Style::make_image(d.widget, d.base_size);   // full icon, no circular crop
            Engine::ParamsSetColorAndOpacity col{Style::icon_is_glyph(d.icon)
                                                    ? d.color
                                                    : Engine::FLinearColor_{1.0f, 1.0f, 1.0f, 1.0f}};
            Engine::call(d.widget, L"SetColorAndOpacity", col);
            d.icon_applied = true;
            return true;
        }

        // One sweep of the pool, painting at most `budget`. `cursor` is carried across
        // calls so successive passes resume rather than restart. A sweep visits each
        // dot at most once, so SIZE_MAX means "paint everything eligible, now".
        auto paint_pass(size_t budget, bool visible_only, size_t& cursor) -> size_t
        {
            size_t painted = 0, scanned = 0;
            const size_t n = m_dots.size();
            while (scanned < n && painted < budget)
            {
                Dot& d = m_dots[cursor % n];
                ++cursor;
                ++scanned;
                if (visible_only && (!is_layer_on(d.layer_id) || d.base_hidden))
                {
                    continue;
                }
                painted += paint_one(d) ? 1 : 0;
            }
            return painted;
        }

        // Paint at most `budget` pending icon textures per call (non-blocking).
        // Runs across ticks so placement stays instant and icons pop in smoothly;
        // painting all of them inline froze the game for upstream.
        //
        // Visible dots first. Only ~1.1k of ~7.6k dots are on at once, so a flat
        // round-robin spent most of its budget on dots nobody could see -- which is
        // what made the map open as colour squares for ~19 ticks. The background pass
        // still runs afterwards, so a layer toggled on later is already painted and
        // pops in clean instead of flashing squares of its own.
        //
        // Loading happens on the game thread (see tick_load_icons); this only
        // reads the finished index, and only once m_icons_ready says the writes
        // are published.
        auto paint_icons_batch(size_t budget) -> void
        {
            if (!g_icons_enabled || m_dots.empty() || !m_icons_ready.load(std::memory_order_acquire))
            {
                return;
            }
            size_t painted = paint_pass(budget, true, m_icon_scan_vis);
            if (painted < budget)
            {
                painted += paint_pass(budget - painted, false, m_icon_scan);
            }
            if (painted > 0 && !m_logged_paint)
            {
                m_logged_paint = true;
                Output::send<LogLevel::Default>(STR("[Lodestone] icon paint: {} dots painted\n"), painted);
            }
        }

        // Render-transform scale for a dot at the current map zoom.
        //
        // Four in-game measurements, each one killing a model:
        //
        //   slot 4px   (clamp(14/z,4,40))  -> tiny + blurry
        //   slot 22px  (clamp(14*sqrt z))  -> grey slabs
        //   slot 1.9px (14*z^-0.75)        -> dots VANISH at z=14.2
        //   slot 14px  (14*z^0)            -> ~200px slabs at z=14.2
        //
        // The last two settle it: rendered = slot * zoom (the map's transform DOES
        // scale us), and a sub-pixel slot is culled by Slate before it ever renders.
        // Those two facts together make SetSize unusable for this -- a constant 14px
        // on screen at z=14.2 needs a ~1px slot, which is exactly the size that
        // disappears.
        //
        // So stop resizing the slot. Keep it fixed at base_size and counter-scale with
        // a render transform, which scales about the centre pivot and therefore does
        // not move the dot off its point:
        //
        //     rendered = base * zoom * scale
        //     scale    = DotScale * zoom^(DotGrowth - 1)
        //     => rendered = base * DotScale * zoom^DotGrowth
        //
        // DotGrowth = 0 gives a constant-size pin at every zoom. The slot stays 14px
        // regardless, so nothing is ever culled for being too small.
        static auto dot_render_scale(double zoom) -> double
        {
            const double z = zoom > 0.01 ? zoom : 1.0;
            return std::clamp(g_dot_scale * std::pow(z, g_dot_growth - 1.0), 0.01, 8.0);
        }

        auto current_zoom(UObject* mask) -> double
        {
            double zoom = 1.0;
            UObject* w = mask;
            for (int i = 0; i < 6 && w; ++i)
            {
                if (auto* xf = w->GetValuePtrByPropertyNameInChain<double>(STR("RenderTransform")))
                {
                    const double sx = xf[2];   // Translation(2d), then Scale.X
                    if (sx > 0.01 && sx < 100.0)
                    {
                        zoom *= sx;
                    }
                }
                struct
                {
                    UObject* ReturnValue{};
                } parent{};
                if (!Engine::call(w, L"GetParent", parent))
                {
                    break;
                }
                w = parent.ReturnValue;
            }
            return zoom;
        }

        auto sync_dot_scale(UObject* mask) -> void
        {
            const double zoom = current_zoom(mask);
            if (zoom <= 0.0 || std::abs(zoom - m_applied_zoom) / m_applied_zoom < 0.05)
            {
                return;
            }
            m_applied_zoom = zoom;
            const double scale = dot_render_scale(zoom);
            if (m_zoom_log_budget > 0)
            {
                --m_zoom_log_budget;
                Output::send<LogLevel::Default>(STR("[Lodestone] zoom={} -> scale {} -> ~{}px on screen\n"),
                                                zoom, scale, 14.0 * zoom * scale);
            }
            for (const auto& d : m_dots)
            {
                if (!d.slot)
                {
                    continue;
                }
                Engine::ParamsSetRenderScale rs{{scale, scale}};
                Engine::call(d.widget, L"SetRenderScale", rs);
            }
        }

        // Find a full-screen canvas to host the panel (the map body root is
        // inset by the decorative frame; WBP_Map_Base's root spans the screen).
        //
        // Visibility must NOT decide identity. WBP_Map_Base outlives the map body --
        // it survives closing and reopening the map -- but it is not visible on every
        // tick. Treating "not visible right now" as "not there" made this flap, and a
        // flap here means the panel gets rebuilt on a different root while the old
        // canvas is still alive on this one: two panels, only the newest polled. So
        // prefer a visible base, but accept an invisible one rather than fall back to
        // a root with a different lifetime.
        // The screen-fixed canvas to hang the panel on: WBP_Map_Base_C's root.
        //
        // Returns nullptr, NOT a fallback, when no base is available. The fallback
        // used to be the map body's root, and that is what doubled the legend twice:
        // WBP_Map_Base_C dies and respawns when the map is rebuilt, so for a tick or
        // two FindAllOf sees none, we commit to the body root instead, and now there
        // is a panel on a DIFFERENT lifetime that outlives nothing and gets orphaned
        // the moment the base returns. Skipping the build for one tick costs nothing;
        // the body root was never a correct parent anyway -- it pans with the map.
        auto screen_canvas() -> UObject*
        {
            std::vector<UObject*> bases;
            UObjectGlobals::FindAllOf(STR("WBP_Map_Base_C"), bases);
            UObject* stored_root = nullptr;   // the base we already built on, if still alive
            bool stored_visible = false;      // ...and whether it is the one on screen now
            UObject* visible = nullptr;
            UObject* invisible = nullptr;
            for (auto* base : bases)
            {
                if (!base)
                {
                    continue;
                }
                auto** tree = base->GetValuePtrByPropertyNameInChain<UObject*>(STR("WidgetTree"));
                if (!tree || !*tree)
                {
                    continue;
                }
                auto** r = (*tree)->GetValuePtrByPropertyNameInChain<UObject*>(STR("RootWidget"));
                if (!r || !*r || Engine::class_name(*r) != L"CanvasPanel")
                {
                    continue;
                }
                Engine::ParamsIsVisible vis{};
                const bool is_vis = Engine::call(base, L"IsVisible", vis) && vis.ReturnValue;
                // Comparing NAMES, not pointers: the stored name is a string, so this is
                // safe even if the old canvas has been collected.
                if (!m_panel_root_name.empty() && (*r)->GetFullName() == m_panel_root_name)
                {
                    stored_root = *r;
                    stored_visible = is_vis;
                }
                if (is_vis)
                {
                    if (!visible)
                    {
                        visible = *r;
                    }
                }
                else if (!invisible)
                {
                    invisible = *r;
                }
            }
            // STICKY (common case): the base we already built on is still the one on
            // screen -- keep it. Do NOT migrate while ours is visible; that is what
            // orphaned the old panel and cost two doubled legends.
            if (stored_root && stored_visible)
            {
                return stored_root;
            }
            // ORPHANED: our base is still alive but no longer visible, while a DIFFERENT
            // base is now shown. Leaving + re-entering the map (or a server world
            // transition) tears down the old WBP_Map_Base and builds a fresh one; the
            // old instance lingers un-collected, so plain stickiness pins us to a hidden
            // base forever and the legend never returns on reopen (Kenny, 2026-07-18).
            // Migrate to the visible base. build_panel rebuilds there and its named
            // sweep keeps the move from doubling. Migrate ONLY when a base is actually
            // visible -- a bare invisible flicker mid-rebuild must not orphan us, so fall
            // through to holding the stored base in that case.
            if (visible)
            {
                if (stored_root)
                {
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] panel: map base swapped, migrating off the hidden one\n"));
                }
                return visible;
            }
            if (stored_root)
            {
                return stored_root;   // nothing visible this tick: hold, do not orphan
            }
            return invisible;   // may be null: caller waits rather than guessing
        }

        // Build the interface panel once per map instance, in a screen-fixed
        // canvas (not the panning map canvas). Rows: [checkbox][dot][label].
        auto build_panel(UObject* root_in) -> void
        {
            if (!root_in)
            {
                return;
            }
            UObject* root = screen_canvas();
            if (!root)
            {
                // No map base this tick. Wait for one; never fall back to the map body
                // root, which is a different lifetime and pans with the map.
                return;
            }
            // Key the panel's lifetime on its OWN root, not the map body's. Same root
            // => the panel we built is still alive and still parented; keep polling it
            // and do not build a second one.
            //
            // A changed root now means something screen_canvas could not find our old
            // root -- i.e. that widget tree is really gone and took our canvas with
            // it. (Before the sticky check it could also mean "we changed our mind",
            // which left a live orphan on a live root: the doubled legend.) So the
            // pointers can be dropped WITHOUT touching them -- they may already be
            // collected -- and rebuilt below.
            const std::wstring root_name = root->GetFullName();
            if (m_panel_canvas)
            {
                if (m_panel_root_name == root_name)
                {
                    return;
                }
                m_panel_canvas = nullptr;
                m_panel_slot = nullptr;
                m_panel_rows.clear();
                m_panel_cats.clear();
                m_panel_relayout = false;
                m_panel_first_poll = true;
            }
            auto* canvas_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
            if (!canvas_class)
            {
                return;
            }
            // Sweep any stale panel left on THIS root before building a fresh one.
            // We only ever reach here on a first build or a migrate, and only for the
            // live base screen_canvas just returned, so touching its children is
            // dangling-safe. A base reused across map opens (or the one we migrate back
            // onto) can still carry our previous panel; without this sweep the migrate
            // would stack a second legend -- the doubled-icon lesson, applied to the
            // panel. Prefix match because StaticConstructObject may suffix the name
            // ("_1") while a just-swept panel still holds it pending GC.
            {
                std::vector<UObject*> stale;
                const int32_t nchild = Engine::children_count(root);
                for (int32_t i = 0; i < nchild; ++i)
                {
                    UObject* c = Engine::child_at(root, i);
                    if (c && Engine::widget_name(c).starts_with(L"Lodestone_Panel"))
                    {
                        stale.push_back(c);
                    }
                }
                for (UObject* s : stale)
                {
                    Engine::ParamsRemoveChild rc{s, false};
                    Engine::call(root, L"RemoveChild", rc);
                }
                if (!stale.empty())
                {
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] swept {} stale panel(s) off a reused map base\n"), stale.size());
                }
            }
            // panel container
            {
                FStaticConstructObjectParameters params{canvas_class, root};
                params.Name = FName(STR("Lodestone_Panel"));
                m_panel_canvas = UObjectGlobals::StaticConstructObject(params);
            }
            if (!m_panel_canvas)
            {
                return;
            }
            Engine::ParamsAddChildToCanvas add{m_panel_canvas, nullptr};
            if (!Engine::call(root, L"AddChildToCanvas", add) || !add.ReturnValue)
            {
                m_panel_canvas = nullptr;
                return;
            }
            m_panel_slot = add.ReturnValue;
            {
                // Top-RIGHT, not top-left: vanilla draws the map's live coordinate
                // readout in the top-left corner, and the panel sat on top of it.
                // Widening the panel for the distance column (190 -> 250) made that
                // worse, and the coord readout is not decoration -- it is the only
                // way to check a "found X at map (a,b)" report against our data.
                //
                // Anchor and alignment both go to the right edge so the panel grows
                // leftward: with alignment X = 1 the offset is measured to its RIGHT
                // edge, so kPanelMargin is a true margin at any resolution and any
                // panel width. (Point anchor => Offsets is {X, Y, Width, Height}.)
                Engine::ParamsSetAnchors anch{1, 0, 1, 0};
                Engine::call(m_panel_slot, L"SetAnchors", anch);
                Engine::ParamsSetAlignment align{{1.0, 0.0}};
                Engine::call(m_panel_slot, L"SetAlignment", align);
            }
            // Log the root's identity, not a label. The old line printed
            // "map-base-screen" for BOTH a first build and a rebuild onto a different
            // base instance, so a doubled panel looked identical to a healthy one --
            // the name is what distinguishes them, and a second line here at all now
            // means a rebuild happened and is worth explaining.
            Output::send<LogLevel::Default>(STR("[Lodestone] panel parent: {} (build #{})\n"), root_name,
                                            ++m_panel_builds);
            m_panel_root_name = root->GetFullName();
            layout_panel();
        }

        // (Re)build the panel's contents inside the existing canvas. Runs on the
        // first build and again on every fold, because folding changes both which
        // rows exist and the panel's height -- so the slot's offsets have to be
        // re-pushed, not just the widgets rebuilt. Rebuilding beats hiding here: a
        // folded row then costs no height, and the widget count is small enough
        // (tens, not the thousands the dot pool deals with) that pooling is noise.
        auto layout_panel() -> void
        {
            if (!m_panel_canvas || !m_panel_slot)
            {
                return;
            }
            auto* canvas_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            auto* cb_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CheckBox"));
            auto* txt_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
            if (!canvas_class || !image_class || !cb_class || !txt_class)
            {
                return;
            }
            Engine::ParamsNone none{};
            Engine::call(m_panel_canvas, L"ClearChildren", none);
            m_panel_rows.clear();
            m_panel_cats.clear();
            m_card_canvas = nullptr;   // a child of m_panel_canvas: died with ClearChildren
            m_card_slot = nullptr;
            m_relic_name_lbl = nullptr;   // header-label refs die with the cleared children
            m_pal_name_lbl = nullptr;
            // Every checkbox below is seeded from our own state, so the next poll
            // has to adopt what it reads rather than treat it as a user toggle.
            m_panel_first_poll = true;

            // Menu closed: draw the compact cog launcher (top-right) and stop. The whole
            // panel is just this one button + its invisible click target -- no tabs, no card.
            if (!m_menu_open)
            {
                render_launcher_cog(image_class, cb_class);
                return;
            }

            const bool modal = m_menu_open;
            const auto items = panel_items();
            auto item_h = [](const PanelItem& it) -> double {
                switch (it.kind)
                {
                case PanelItem::Title:
                    return 30.0;
                case PanelItem::Header:
                    return 22.0;   // includes top gap before the header
                default:
                    return 19.0;
                }
            };
            // Accordions are gone (2026-07-19): the menu is a tabbed modal, so every row of
            // the active tab shows. Kept for render-loop index parity with the item vector.
            std::vector<bool> shown(items.size(), true);

            // Metrics. The modal card is wider than the old right rail and lays its menu items
            // (Pick / Button / Tab) in a 2-column grid to stay compact; a wide Pill spans the
            // whole width. Tabs sit three-across at the top.
            const double width = modal ? 460.0 : 280.0;
            const double padX = modal ? 14.0 : 8.0, gap = 6.0, pillH = 28.0, rgap = 6.0;
            const double cellW = (width - 2.0 * padX - gap) / 2.0;
            const double fullW = width - 2.0 * padX;
            const double pickH = 26.0, buttonH = 32.0, chipW = 26.0, chipH = 24.0, chipGap = 4.0;
            const double tabH = 30.0, tabGap = 6.0, topPad = 6.0;
            struct Slot
            {
                double x, y, w, h;
            };
            std::vector<Slot> slot(items.size(), {0, 0, 0, 0});
            double content_h = topPad;
            {
                double yy = topPad;
                int col = 0;                // 0/1 within the 2-column grid
                double grid_rowH = pillH;   // height of the current grid row (per style)
                double chipx = -1.0;        // >=0 while mid A-Z chip-run
                int tab_col = 0;            // 0..2 across the tab bar
                auto close_grid = [&]() {
                    if (col == 1)
                    {
                        yy += grid_rowH + rgap;
                        col = 0;
                    }
                };
                auto close_chips = [&]() {
                    if (chipx >= 0)
                    {
                        yy += chipH + rgap;
                        chipx = -1.0;
                    }
                };
                auto close_tabs = [&]() {
                    if (tab_col > 0)
                    {
                        yy += tabH + gap;
                        tab_col = 0;
                    }
                };
                auto close_runs = [&]() {
                    close_grid();
                    close_chips();
                    close_tabs();
                };
                for (size_t i = 0; i < items.size(); ++i)
                {
                    if (!shown[i])
                    {
                        continue;
                    }
                    const auto& it = items[i];
                    if (it.kind != PanelItem::Row)   // Title / Header: full width, closes any run
                    {
                        close_runs();
                        slot[i] = {padX, yy, fullW, item_h(it)};
                        yy += item_h(it);
                        continue;
                    }
                    if (it.style == PanelItem::Tab)
                    {
                        close_grid();
                        close_chips();
                        const double tabW = (fullW - 2.0 * tabGap) / 3.0;
                        slot[i] = {padX + tab_col * (tabW + tabGap), yy, tabW, tabH};
                        if (++tab_col == 3)
                        {
                            yy += tabH + gap;
                            tab_col = 0;
                        }
                        continue;
                    }
                    close_tabs();
                    if (it.style == PanelItem::Chip)
                    {
                        close_grid();
                        if (chipx < 0)
                        {
                            chipx = padX;
                        }
                        else if (chipx + chipW > width - padX)   // wrap the rail
                        {
                            yy += chipH + chipGap;
                            chipx = padX;
                        }
                        slot[i] = {chipx, yy, chipW, chipH};
                        chipx += chipW + chipGap;
                        continue;
                    }
                    close_chips();
                    // A wide Pill (the effigy/pal toggle) or wide Pick (a hint line) spans the
                    // whole width; everything else grids 2-up in the modal. The launcher (not
                    // modal) has only a Title + one Button, so the grid never triggers there.
                    const bool grid2 = !it.wide &&
                                       (it.style == PanelItem::Pill ||
                                        (modal && (it.style == PanelItem::Pick || it.style == PanelItem::Button)));
                    if (grid2)
                    {
                        grid_rowH = (it.style == PanelItem::Pill)
                                        ? pillH
                                        : (it.style == PanelItem::Button ? buttonH : pickH);
                        slot[i] = {(col == 0) ? padX : (padX + cellW + gap), yy, cellW, grid_rowH};
                        if (col == 0)
                        {
                            col = 1;
                        }
                        else
                        {
                            col = 0;
                            yy += grid_rowH + rgap;
                        }
                    }
                    else   // full-width row (wide Pill/Pick, or a launcher Button)
                    {
                        close_grid();
                        const double h = (it.style == PanelItem::Button)
                                             ? buttonH
                                             : (it.style == PanelItem::Pick ? pickH : pillH);
                        slot[i] = {padX, yy, fullW, h};
                        yy += h + rgap;
                    }
                }
                close_runs();
                content_h = yy;
            }
            const double height = content_h + 8.0;   // bottom padding

            // ---- Mode setup: right-rail launcher vs full-screen modal ----------------------
            UObject* target = m_panel_canvas;
            if (modal)
            {
                // The panel canvas fills the screen so the backdrop can cover the map.
                Engine::ParamsSetAnchors fa{0, 0, 1, 1};
                Engine::call(m_panel_slot, L"SetAnchors", fa);
                Engine::ParamsSetAlignment a0{{0.0, 0.0}};
                Engine::call(m_panel_slot, L"SetAlignment", a0);
                Engine::ParamsSetOffsets fo{0.0f, 0.0f, 0.0f, 0.0f};
                Engine::call(m_panel_slot, L"SetOffsets", fo);
                // Opaque backdrop: hides the map AND (Vis_Visible, not HitTestInvisible) eats
                // clicks so nothing reaches the map under it. Anchor-filled, not px-sized.
                {
                    FStaticConstructObjectParameters bp{image_class, m_panel_canvas};
                    if (UObject* back = UObjectGlobals::StaticConstructObject(bp))
                    {
                        Style::make_round(back, 0.0);
                        Engine::ParamsSetColorAndOpacity c{{0.02f, 0.03f, 0.06f, 0.97f}};
                        Engine::call(back, L"SetColorAndOpacity", c);
                        Engine::ParamsSetVisibility v{Engine::Vis_Visible};
                        Engine::call(back, L"SetVisibility", v);
                        Engine::ParamsAddChildToCanvas add{back, nullptr};
                        if (Engine::call(m_panel_canvas, L"AddChildToCanvas", add) && add.ReturnValue)
                        {
                            Engine::ParamsSetAnchors ba{0, 0, 1, 1};
                            Engine::call(add.ReturnValue, L"SetAnchors", ba);
                            Engine::ParamsSetOffsets bo{0.0f, 0.0f, 0.0f, 0.0f};
                            Engine::call(add.ReturnValue, L"SetOffsets", bo);
                        }
                    }
                }
                // Centred card canvas -- a centre anchor + centre alignment centres it with no
                // viewport-size query. Its children are positioned card-relative.
                {
                    FStaticConstructObjectParameters cp{canvas_class, m_panel_canvas};
                    cp.Name = FName(STR("Lodestone_Card"));
                    m_card_canvas = UObjectGlobals::StaticConstructObject(cp);
                }
                if (m_card_canvas)
                {
                    Engine::ParamsAddChildToCanvas add{m_card_canvas, nullptr};
                    if (Engine::call(m_panel_canvas, L"AddChildToCanvas", add) && add.ReturnValue)
                    {
                        m_card_slot = add.ReturnValue;
                        Engine::ParamsSetAnchors ca{0.5f, 0.5f, 0.5f, 0.5f};
                        Engine::call(m_card_slot, L"SetAnchors", ca);
                        Engine::ParamsSetAlignment cal{{0.5, 0.5}};
                        Engine::call(m_card_slot, L"SetAlignment", cal);
                        Engine::ParamsSetOffsets co{0.0f, 0.0f, static_cast<float>(width),
                                                    static_cast<float>(height)};
                        Engine::call(m_card_slot, L"SetOffsets", co);
                        target = m_card_canvas;
                    }
                }
                if (target == m_panel_canvas)
                {
                    return;   // card build failed; do not dump the menu onto the backdrop
                }
            }
            else
            {
                // Right rail: anchored + aligned to the top-right, grows leftward. X is
                // negative because the slot is aligned to the right edge (see build_panel).
                Engine::ParamsSetAnchors anch{1, 0, 1, 0};
                Engine::call(m_panel_slot, L"SetAnchors", anch);
                Engine::ParamsSetAlignment align{{1.0, 0.0}};
                Engine::call(m_panel_slot, L"SetAlignment", align);
                Engine::ParamsSetOffsets offs{static_cast<float>(-kPanelMargin), static_cast<float>(kPanelMargin),
                                              static_cast<float>(width), static_cast<float>(height)};
                Engine::call(m_panel_slot, L"SetOffsets", offs);
            }

            // Helpers target the ACTIVE canvas (card in modal, panel otherwise).
            auto add_to_panel = [&](UObject* w, double x, double y, double w_, double h_) -> UObject* {
                Engine::ParamsAddChildToCanvas a{w, nullptr};
                if (!Engine::call(target, L"AddChildToCanvas", a) || !a.ReturnValue)
                {
                    return nullptr;
                }
                Engine::ParamsSetAlignment al{{0.0, 0.0}};
                Engine::call(a.ReturnValue, L"SetAlignment", al);
                Engine::ParamsSetAutoSize aut{false};
                Engine::call(a.ReturnValue, L"SetAutoSize", aut);
                Engine::ParamsSetSize sz{{w_, h_}};
                Engine::call(a.ReturnValue, L"SetSize", sz);
                Engine::ParamsSetPosition p{{x, y}};
                Engine::call(a.ReturnValue, L"SetPosition", p);
                return a.ReturnValue;
            };
            // Card / panel background.
            {
                FStaticConstructObjectParameters params{image_class, target};
                UObject* bg = UObjectGlobals::StaticConstructObject(params);
                if (bg)
                {
                    if (modal)
                    {
                        // Gold-bordered card, near-opaque, over the dark backdrop.
                        Style::make_round(bg, 12.0, 2.0, Style::kAccentR, Style::kAccentG, Style::kAccentB, 0.85f);
                        Engine::ParamsSetColorAndOpacity c{{0.055f, 0.098f, 0.16f, 0.99f}};
                        Engine::call(bg, L"SetColorAndOpacity", c);
                    }
                    else
                    {
                        Style::make_round(bg, 8.0);
                        Engine::ParamsSetColorAndOpacity c{{0.055f, 0.098f, 0.16f, 0.94f}};
                        Engine::call(bg, L"SetColorAndOpacity", c);
                    }
                    Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                    Engine::call(bg, L"SetVisibility", v);
                    add_to_panel(bg, 0, 0, width, height);
                }
            }
            // A thin label helper (title / header / row text). Returns the widget so callers
            // that retext it later (the dynamic headers) can keep it.
            auto add_label = [&](const wchar_t* text, double x, double y, double w_, int font, float r, float g,
                                 float b, float a) -> UObject* {
                FStaticConstructObjectParameters tp{txt_class, target};
                UObject* txt = UObjectGlobals::StaticConstructObject(tp);
                if (!txt)
                {
                    return nullptr;
                }
                Engine::ParamsSetText st{FText(text)};
                Engine::call(txt, L"SetText", st);
                Style::set_font_size(txt, font);
                // FSlateColor, not FLinearColor -- rule 0 pins the colour so a label can't
                // flip to the foreground default.
                Engine::ParamsSetTextColor tc{{r, g, b, a}, 0};
                Engine::call(txt, L"SetColorAndOpacity", tc);
                Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                Engine::call(txt, L"SetVisibility", v);
                add_to_panel(txt, x, y, w_, 16);
                return txt;
            };
            // A 1px separator line.
            auto add_rule = [&](double x, double y, double w_) {
                FStaticConstructObjectParameters params{image_class, target};
                UObject* line = UObjectGlobals::StaticConstructObject(params);
                if (!line)
                {
                    return;
                }
                Style::make_round(line);
                Engine::ParamsSetColorAndOpacity c{{1.0f, 1.0f, 1.0f, 0.12f}};
                Engine::call(line, L"SetColorAndOpacity", c);
                Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                Engine::call(line, L"SetVisibility", v);
                add_to_panel(line, x, y, w_, 1.0);
            };

            for (size_t i = 0; i < items.size(); ++i)
            {
                const auto& it = items[i];
                if (!shown[i])
                {
                    continue;
                }
                const Slot& s = slot[i];
                if (it.kind == PanelItem::Title)
                {
                    add_label(it.label, s.x + 4, s.y + 4, s.w - 8, modal ? 16 : 15, 0.96f, 0.97f, 1.0f, 1.0f);
                    add_rule(s.x + 2, s.y + 26, s.w - 4);
                    continue;
                }
                if (it.kind == PanelItem::Header)
                {
                    // A plain section label (no checkbox -- accordions are gone). An empty
                    // label is the footer divider: just the rule, closing the grid above it.
                    if (it.label && it.label[0] != L'\0')
                    {
                        add_label(it.label, s.x + 2, s.y + 8, s.w - 6, 9, 0.60f, 0.69f, 0.84f, 1.0f);
                    }
                    add_rule(s.x + 2, s.y + item_h(it) - 3, s.w - 4);
                    continue;
                }
                const double rh = s.h > 0 ? s.h : pillH;
                // Flat-card looks: Pick (menu item), Chip (A-Z rail), Button (action), Tab
                // (active-highlighted). Highlight comes from `selected`, NOT is_layer_on.
                if (it.style != PanelItem::Pill)
                {
                    const bool sel = it.selected;
                    const bool button_like = (it.style == PanelItem::Button || it.style == PanelItem::Tab);
                    UObject* flat_card = nullptr;
                    {
                        FStaticConstructObjectParameters bp{image_class, target};
                        if (UObject* card = UObjectGlobals::StaticConstructObject(bp))
                        {
                            Style::make_round(card, it.style == PanelItem::Chip ? 6.0 : 7.0);
                            Engine::ParamsSetColorAndOpacity c{pick_fill(button_like, sel)};
                            Engine::call(card, L"SetColorAndOpacity", c);
                            Engine::ParamsSetVisibility cv{Engine::Vis_HitTestInvisible};
                            Engine::call(card, L"SetVisibility", cv);
                            add_to_panel(card, s.x, s.y, s.w, rh);
                            flat_card = card;
                        }
                    }
                    // Optional icon at a Pick row's left (effigy type -> its own Pal-statue
                    // icon, so the picker matches the map). Reserve the pad whenever the row
                    // carries an icon so the layout is stable even before textures finish
                    // loading; draw the art once it is ready.
                    const double icon_pad = (it.icon && it.style == PanelItem::Pick) ? 20.0 : 0.0;
                    if (icon_pad > 0.0)
                    {
                        FStaticConstructObjectParameters sp{image_class, target};
                        if (UObject* sw = UObjectGlobals::StaticConstructObject(sp))
                        {
                            UObject* tex = m_icons_ready.load(std::memory_order_acquire) ? layer_texture(it.icon)
                                                                                         : nullptr;
                            if (tex)
                            {
                                Engine::ParamsSetBrushFromTexture b{tex, false};
                                Engine::call(sw, L"SetBrushFromTexture", b);
                                Style::make_image(sw, 16.0);
                                Engine::ParamsSetColorAndOpacity c{
                                    Style::icon_is_glyph(it.icon) ? Engine::FLinearColor_{it.r, it.g, it.b, 1.0f}
                                                                  : Engine::FLinearColor_{1.0f, 1.0f, 1.0f, 1.0f}};
                                Engine::call(sw, L"SetColorAndOpacity", c);
                                Engine::ParamsSetVisibility iv{Engine::Vis_HitTestInvisible};
                                Engine::call(sw, L"SetVisibility", iv);
                                add_to_panel(sw, s.x + 8.0, s.y + (rh - 16.0) / 2.0, 16.0, 16.0);
                            }
                        }
                    }
                    // label: left-aligned for Pick (the real name), centred otherwise
                    float lr = 0.80f, lg = 0.85f, lb = 0.92f;
                    if (button_like && sel)
                    {
                        lr = 0.06f;
                        lg = 0.06f;
                        lb = 0.06f;   // dark text on the amber active Tab / Apply
                    }
                    else if (sel)
                    {
                        lr = 1.0f;
                        lg = 1.0f;
                        lb = 1.0f;
                    }
                    const int font = it.style == PanelItem::Chip ? 10 : (button_like ? 12 : 11);
                    const double lx = (it.style == PanelItem::Pick) ? s.x + 10.0 + icon_pad : s.x + 3.0;
                    const double lw = s.w - ((it.style == PanelItem::Pick) ? 16.0 + icon_pad : 6.0);
                    if (UObject* lbl = add_label(it.label, lx, s.y + (rh - 16.0) / 2.0, (lw > 8.0 ? lw : 8.0), font,
                                                 lr, lg, lb, 1.0f))
                    {
                        Engine::ParamsSetClipping clip{1};   // EWidgetClipping::ClipToBounds
                        Engine::call(lbl, L"SetClipping", clip);
                        if (it.style != PanelItem::Pick)
                        {
                            Engine::ParamsSetJustification jc{1};   // ETextJustify::Center
                            Engine::call(lbl, L"SetJustification", jc);
                        }
                    }
                    {
                        FStaticConstructObjectParameters cbp{cb_class, target};
                        if (UObject* cb = UObjectGlobals::StaticConstructObject(cbp))
                        {
                            Style::make_checkbox_clear(cb);
                            Engine::ParamsSetVisibility v{Engine::Vis_Visible};
                            Engine::call(cb, L"SetVisibility", v);
                            add_to_panel(cb, s.x, s.y, s.w, rh);
                            if (auto* st = cb->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CheckedState")))
                            {
                                *st = 0;   // seeded unchecked; a click flips it -> poll fires the id
                            }
                            Engine::ParamsSetIsChecked chk{false};
                            Engine::call(cb, L"SetIsChecked", chk);
                            m_panel_rows.push_back({cb, it.id, false, nullptr, flat_card});
                        }
                    }
                    continue;
                }
                // toggle row = a palworld.gg pill. The pill CARD (dim off / brighter + a teal
                // ring on) is a full-cell Image, drawn first (behind). Icon + name + count go
                // on top HitTestInvisible, and a full-cell invisible CheckBox (added last,
                // catches the click) drives the toggle poll. In the menu the pill reflects the
                // STAGED state -- the map only changes on Apply.
                const bool row_on = m_menu_open ? stage_on(it.id) : is_layer_on(it.id);
                UObject* pill_card = nullptr;
                {
                    FStaticConstructObjectParameters bp{image_class, target};
                    if (UObject* card = UObjectGlobals::StaticConstructObject(bp))
                    {
                        Style::make_pill_bg(card, row_on);
                        Engine::ParamsSetColorAndOpacity w{{1.0f, 1.0f, 1.0f, 1.0f}};   // let the brush colours show
                        Engine::call(card, L"SetColorAndOpacity", w);
                        Engine::ParamsSetVisibility cv{Engine::Vis_HitTestInvisible};
                        Engine::call(card, L"SetVisibility", cv);
                        add_to_panel(card, s.x, s.y, s.w, rh);
                        pill_card = card;
                    }
                }
                // icon (or colour swatch) at the pill's left
                {
                    FStaticConstructObjectParameters sp{image_class, target};
                    if (UObject* sw = UObjectGlobals::StaticConstructObject(sp))
                    {
                        UObject* tex = m_icons_ready.load(std::memory_order_acquire) ? layer_texture(it.icon)
                                                                                     : nullptr;
                        if (tex)
                        {
                            Engine::ParamsSetBrushFromTexture b{tex, false};
                            Engine::call(sw, L"SetBrushFromTexture", b);
                            Style::make_image(sw, 16.0);
                            Engine::ParamsSetColorAndOpacity c{Style::icon_is_glyph(it.icon)
                                                                  ? Engine::FLinearColor_{it.r, it.g, it.b, 1.0f}
                                                                  : Engine::FLinearColor_{1.0f, 1.0f, 1.0f, 1.0f}};
                            Engine::call(sw, L"SetColorAndOpacity", c);
                        }
                        else
                        {
                            Style::make_round(sw, 8.0);
                            Engine::ParamsSetColorAndOpacity c{{it.r, it.g, it.b, 1.0f}};
                            Engine::call(sw, L"SetColorAndOpacity", c);
                        }
                        Engine::ParamsSetVisibility sv{Engine::Vis_HitTestInvisible};
                        Engine::call(sw, L"SetVisibility", sv);
                        add_to_panel(sw, s.x + 6, s.y + (rh - 16.0) / 2.0, 16.0, 16.0);
                    }
                }
                // name (fills the middle) + count (right), both HitTestInvisible.
                // Layers with a real "collected" state show found/total (e.g. Effigies 12/407,
                // Fast Travel 22/174); everything else shows a plain total.
                const int found = layer_found(it.id);
                const std::wstring cs =
                    (found >= 0)
                        ? std::to_wstring(found) + L"/" + std::to_wstring(layer_total(it.id))
                        : std::to_wstring(layer_count(it.id));
                const double countW = 8.0 + 7.0 * static_cast<double>(cs.size());
                const double nameX = s.x + 26.0;
                const double nameW = s.x + s.w - countW - 6.0 - nameX;
                if (UObject* lbl = add_label(it.label, nameX, s.y + 7, (nameW > 12.0 ? nameW : 12.0), 11,
                                             Style::kAccentR, Style::kAccentG, Style::kAccentB, 1.0f))
                {
                    Engine::ParamsSetClipping clip{1};   // EWidgetClipping::ClipToBounds
                    Engine::call(lbl, L"SetClipping", clip);
                    // Capture the two dynamic headers so a pick can retext them in place.
                    if (it.id == kEffigyLayer)
                    {
                        m_relic_name_lbl = lbl;
                    }
                    else if (it.id == kPalSpeciesLayer)
                    {
                        m_pal_name_lbl = lbl;
                    }
                }
                if (UObject* num = add_label(cs.c_str(), s.x + s.w - countW - 6.0, s.y + 8, countW, 9, 0.66f,
                                             0.74f, 0.88f, 1.0f))
                {
                    Engine::ParamsSetJustification jr{2};   // ETextJustify::Right
                    Engine::call(num, L"SetJustification", jr);
                }
                // invisible full-cell click target on top -- drives the toggle poll
                {
                    FStaticConstructObjectParameters cbp{cb_class, target};
                    if (UObject* cb = UObjectGlobals::StaticConstructObject(cbp))
                    {
                        Style::make_checkbox_clear(cb);
                        Engine::ParamsSetVisibility v{Engine::Vis_Visible};
                        Engine::call(cb, L"SetVisibility", v);
                        add_to_panel(cb, s.x, s.y, s.w, rh);
                        if (auto* st = cb->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CheckedState")))
                        {
                            *st = row_on ? 1 : 0;   // ECheckBoxState::Checked
                        }
                        Engine::ParamsSetIsChecked chk{row_on};
                        Engine::call(cb, L"SetIsChecked", chk);
                        m_panel_rows.push_back({cb, it.id, row_on, nullptr, pill_card});
                    }
                }
            }
            Output::send<LogLevel::Default>(STR("[Lodestone] panel laid out: {} rows ({})\n"),
                                            m_panel_rows.size(), modal ? L"modal" : L"launcher");
            // These rows are brand new -- their distance column is empty until filled. Every
            // path that rebuilds rows comes through here, so this keeps the readout alive.
            refresh_nearest();
        }

        // The CLOSED launcher: a single cog button, top-right of the open map. Replaces the
        // old "Configure map" text button so the entry point feels part of the map. Clicking
        // it (or F5) opens the config popup via the existing kMenuOpen click path. The cog is
        // the Unicode gear glyph U+2699 in tehAon amber on a bordered backing; if the game
        // font lacks the glyph it shows a box (the drawn-cog fallback is the follow-up).
        auto render_launcher_cog(UClass* image_class, UClass* cb_class) -> void
        {
            const double size = g_menu_btn_size;
            // Top-right point anchor, aligned to its right edge, grows leftward -- same idiom
            // as the full panel. X/Y push the button in from the corner.
            {
                Engine::ParamsSetAnchors anch{1, 0, 1, 0};
                Engine::call(m_panel_slot, L"SetAnchors", anch);
                Engine::ParamsSetAlignment align{{1.0, 0.0}};
                Engine::call(m_panel_slot, L"SetAlignment", align);
                Engine::ParamsSetOffsets offs{static_cast<float>(-(kPanelMargin + g_menu_btn_x)),
                                              static_cast<float>(kPanelMargin + g_menu_btn_y),
                                              static_cast<float>(size), static_cast<float>(size)};
                Engine::call(m_panel_slot, L"SetOffsets", offs);
            }
            auto add = [&](UObject* w, double x, double y, double w_, double h_) -> UObject* {
                Engine::ParamsAddChildToCanvas a{w, nullptr};
                if (!Engine::call(m_panel_canvas, L"AddChildToCanvas", a) || !a.ReturnValue)
                {
                    return nullptr;
                }
                Engine::ParamsSetAlignment al{{0.0, 0.0}};
                Engine::call(a.ReturnValue, L"SetAlignment", al);
                Engine::ParamsSetAutoSize aut{false};
                Engine::call(a.ReturnValue, L"SetAutoSize", aut);
                Engine::ParamsSetSize sz{{w_, h_}};
                Engine::call(a.ReturnValue, L"SetSize", sz);
                Engine::ParamsSetPosition p{{x, y}};
                Engine::call(a.ReturnValue, L"SetPosition", p);
                return a.ReturnValue;
            };
            // backing: dark navy rounded square with a thin amber border
            {
                FStaticConstructObjectParameters bp{image_class, m_panel_canvas};
                if (UObject* bg = UObjectGlobals::StaticConstructObject(bp))
                {
                    Style::make_round(bg, 12.0, 2.0, Style::kAccentR, Style::kAccentG, Style::kAccentB, 0.9f);
                    Engine::ParamsSetColorAndOpacity c{{0.055f, 0.098f, 0.16f, 0.96f}};
                    Engine::call(bg, L"SetColorAndOpacity", c);
                    Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                    Engine::call(bg, L"SetVisibility", v);
                    add(bg, 0, 0, size, size);
                }
            }
            // Drawn cog (font-independent): 8 amber tooth squares, an amber disc on top,
            // then a navy hole -> reads as a gear ring. The U+2699 glyph is absent from
            // Palworld's font (it drew the .notdef box), so we draw the cog ourselves.
            {
                const double cx = size / 2.0, cy = size / 2.0;
                const Engine::FLinearColor_ amber{Style::kAccentR, Style::kAccentG, Style::kAccentB, 1.0f};
                auto disc_shape = [&](double x, double y, double d, const Engine::FLinearColor_& col) {
                    FStaticConstructObjectParameters sp{image_class, m_panel_canvas};
                    if (UObject* w = UObjectGlobals::StaticConstructObject(sp))
                    {
                        Style::make_round(w, d / 2.0);
                        Engine::ParamsSetColorAndOpacity c{col};
                        Engine::call(w, L"SetColorAndOpacity", c);
                        Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                        Engine::call(w, L"SetVisibility", v);
                        add(w, x, y, d, d);
                    }
                };
                const double tooth = size * 0.22, tr = size * 0.34;
                for (int i = 0; i < 8; ++i)
                {
                    const double ang = i * (3.14159265358979323846 / 4.0);
                    FStaticConstructObjectParameters tp{image_class, m_panel_canvas};
                    if (UObject* t = UObjectGlobals::StaticConstructObject(tp))
                    {
                        Style::make_round(t, 2.0);   // slightly rounded square tooth
                        Engine::ParamsSetColorAndOpacity c{amber};
                        Engine::call(t, L"SetColorAndOpacity", c);
                        Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                        Engine::call(t, L"SetVisibility", v);
                        add(t, cx + tr * std::cos(ang) - tooth / 2.0, cy + tr * std::sin(ang) - tooth / 2.0,
                            tooth, tooth);
                    }
                }
                disc_shape(cx - size * 0.28, cy - size * 0.28, size * 0.56, amber);            // outer disc
                disc_shape(cx - size * 0.12, cy - size * 0.12, size * 0.24, {0.055f, 0.098f, 0.16f, 1.0f});  // hole
            }
            // invisible full-cell click target -> kMenuOpen (opens the popup on click)
            {
                FStaticConstructObjectParameters cbp{cb_class, m_panel_canvas};
                if (UObject* cb = UObjectGlobals::StaticConstructObject(cbp))
                {
                    Style::make_checkbox_clear(cb);
                    Engine::ParamsSetVisibility v{Engine::Vis_Visible};
                    Engine::call(cb, L"SetVisibility", v);
                    add(cb, 0, 0, size, size);
                    if (auto* stp = cb->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CheckedState")))
                    {
                        *stp = 0;
                    }
                    Engine::ParamsSetIsChecked chk{false};
                    Engine::call(cb, L"SetIsChecked", chk);
                    m_panel_rows.push_back({cb, kMenuOpen, false, nullptr, nullptr});
                }
            }
            Output::send<LogLevel::Default>(STR("[Lodestone] panel laid out: cog launcher\n"));
        }

        // Push the computed distances into the panel's distance column.
        //
        // Separate from compute_nearest because the two have different lifetimes: the
        // numbers are known as soon as the player is located (during place_dots),
        // but the widgets to show them in may not exist yet -- build_panel runs later
        // in the same tick, and rebuilds whenever the map screen is recreated. So
        // this is called after both, and again after any panel rebuild.
        auto refresh_nearest() -> void
        {
            if (!m_have_player)
            {
                return;
            }
            for (auto& row : m_panel_rows)
            {
                if (!row.dist)
                {
                    continue;
                }
                auto it = m_nearest.find(row.layer_id);
                // A layer with no reachable node gets a dash, not a blank: blank
                // reads as "still loading", a dash reads as "none of these left".
                const std::wstring text = (it == m_nearest.end()) ? L"--" : nearest_text(it->second);
                Engine::ParamsSetText st{FText(text.c_str())};
                Engine::call(row.dist, L"SetText", st);
            }
        }

        // Poll checkbox states; on change, update toggle + layer visibility.
        // Dev self-test: exercise the range fast path with no mouse.
        //
        // The mouse cannot be driven on this machine -- the game grabs the pointer and
        // recentres it every frame, so neither xdotool nor ydotool can place it, and
        // the game's own cursor ignores relative motion too. The fast path only runs
        // when a checkbox changes, so it would otherwise ship on a code review alone.
        //
        // First attempt drove UCheckBox::SetIsChecked and waited for poll_panel to
        // notice. The checkbox DID flip on screen and poll_panel never reported a
        // change -- UMG's IsChecked() reads the Slate widget, which evidently does not
        // observe a programmatic set the way it observes a click. Rather than chase
        // that (it is not a path any player takes), do what the poll does on a real
        // click: update the row, update the layer state, and call apply_layers. That
        // tests the code this commit actually changed. The poll -> apply_layers wiring
        // is one line, verified by inspection.
        //
        // The assertion is a number: Coal has 496 points, so a correct range path
        // re-applies 496 dots and NOT the 7,617 in the pool.
        //
        // Dev only: gated on AutoLoadWorld, which real players never have.
        auto dev_selftest_toggle() -> void
        {
            if (g_autoload_world.empty() || m_selftest_done || m_panel_rows.empty())
            {
                return;
            }
            for (auto& row : m_panel_rows)
            {
                if (!row.checkbox || layer_key(row.layer_id) != std::wstring(L"Coal"))
                {
                    continue;
                }
                m_selftest_done = true;
                const bool want = !row.last_checked;
                Engine::ParamsSetIsChecked p{want};
                Engine::call(row.checkbox, L"SetIsChecked", p);   // keep the UI honest
                row.last_checked = want;
                m_layer_on[row.layer_id] = want;
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] selftest: Coal -> {}; expect 496 dots re-applied, not {}\n"),
                    want ? STR("on") : STR("off"), m_dots.size());
                apply_layers({row.layer_id});
                return;
            }
        }

        // Apply queued toggles once the debounce settles (see m_pending_layers). Runs
        // every tick; a dense layer's churn now happens once per burst, not once per click.
        auto drain_pending_apply() -> void
        {
            if (!m_apply_pending || std::chrono::steady_clock::now() < m_apply_at)
            {
                return;
            }
            m_apply_pending = false;
            std::sort(m_pending_layers.begin(), m_pending_layers.end());
            m_pending_layers.erase(std::unique(m_pending_layers.begin(), m_pending_layers.end()),
                                   m_pending_layers.end());
            // A layer turned ON with no dots yet (bounded pool) needs a re-place; else just
            // flip visibility on its existing range. Empty set = a category fold only.
            bool need_replace = false;
            for (int id : m_pending_layers)
            {
                if (is_layer_on(id) && m_layer_ranges.find(id) == m_layer_ranges.end())
                {
                    need_replace = true;
                    break;
                }
            }
            if (need_replace)
            {
                m_placed = false;   // tick re-runs place_dots with the new on-set
            }
            else if (!m_pending_layers.empty())
            {
                apply_layers(m_pending_layers);
            }
            m_pending_layers.clear();
            m_panel_relayout = true;   // redraw pill highlights / apply fold state
        }

        auto poll_panel() -> void
        {
            dev_selftest_toggle();
            bool changed = false;
            int failed = 0, polled = 0;
            for (auto& row : m_panel_rows)
            {
                if (!row.checkbox)
                {
                    ++failed;
                    continue;
                }
                Engine::ParamsIsChecked p{};
                if (!Engine::call(row.checkbox, L"IsChecked", p))
                {
                    ++failed;
                    continue;
                }
                ++polled;
                // UCheckBox::IsChecked() reads the Slate widget when it exists and
                // silently falls back to the CheckedState property otherwise -- and
                // build_panel seeds that property itself. So a dead Slate widget
                // returns our own seed forever and every toggle looks like no
                // change. Compare the two: if they ever disagree, that is the bug.
                if (m_poll_log_budget > 0)
                {
                    if (auto* st = row.checkbox->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CheckedState")))
                    {
                        const bool prop_checked = (*st == 1);
                        if (prop_checked != p.ReturnValue)
                        {
                            --m_poll_log_budget;
                            Output::send<LogLevel::Warning>(
                                STR("[Lodestone] checkbox {} disagrees: IsChecked={} CheckedState={}\n"),
                                row.layer_id, p.ReturnValue, prop_checked);
                        }
                    }
                }
                if (m_panel_first_poll)
                {
                    // build_panel already seeds each checkbox from is_layer_on(), so
                    // the widget mirrors our state here -- adopt it and move on.
                    row.last_checked = p.ReturnValue;
                    continue;
                }
                if (p.ReturnValue != row.last_checked)
                {
                    row.last_checked = p.ReturnValue;
                    // Menu controls (launcher / Apply / Cancel / letter-filter) change the
                    // ROW SET, so they rebuild the panel -- which destroys the widgets this
                    // loop is iterating -- and must return right after.
                    if (handle_menu_ctrl(row.layer_id))
                    {
                        m_panel_relayout = true;
                        return;
                    }
                    // Pal / effigy PICKS only change a highlight (+ a header label), never
                    // the row set, so recolour in place -- NO rebuild (rebuilding on every
                    // pick crashed the game). Still return: we handled the click.
                    if (handle_pal_pick(row.layer_id) || handle_relic_pick(row.layer_id))
                    {
                        update_selection_visuals();
                        return;
                    }
                    // A real layer toggle. The map only changes on Apply, so edit the STAGED
                    // copy here; the pill recolours in place after the loop (no rebuild).
                    m_stage_layer_on[row.layer_id] = p.ReturnValue;
                    changed = true;
                }
            }
            if (m_panel_first_poll)
            {
                m_panel_first_poll = false;
            }
            if (changed)
            {
                update_selection_visuals();   // recolour staged pills in place; map changes only on Apply
            }
            // One-time census: if the row count is wrong, or IsChecked cannot be
            // called at all, no toggle can ever be seen and everything downstream
            // is a red herring.
            if (!m_logged_poll)
            {
                m_logged_poll = true;
                Output::send<LogLevel::Default>(STR("[Lodestone] panel poll: {} rows, {} readable, {} unreadable\n"),
                                                m_panel_rows.size(), polled, failed);
            }
            poll_categories();
        }

        // Fold/unfold. A change rebuilds the panel, which destroys every widget
        // m_panel_rows and m_panel_cats point at -- so it must not happen inside a
        // poll loop. Flag it and let tick() run the relayout between polls.
        auto poll_categories() -> void
        {
            for (auto& cat : m_panel_cats)
            {
                if (!cat.checkbox)
                {
                    continue;
                }
                Engine::ParamsIsChecked p{};
                if (!Engine::call(cat.checkbox, L"IsChecked", p) || p.ReturnValue == cat.last_checked)
                {
                    continue;
                }
                cat.last_checked = p.ReturnValue;
                m_cat_open[cat.key] = p.ReturnValue;
                // Accordion: opening a category collapses every other, so only one
                // category's rows show at a time and the menu can't outgrow the screen.
                if (p.ReturnValue)
                {
                    for (auto& other : m_panel_cats)
                    {
                        if (other.key != cat.key)
                        {
                            m_cat_open[other.key] = false;
                        }
                    }
                }
                save_settings();
                // Debounce the relayout too (empty pending set = fold only), so smashing a
                // category header can't storm layout_panel either.
                m_apply_pending = true;
                m_apply_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                return;   // stop reading this frame; the fold applies after the settle
            }
        }

        // WBP_PlayerUI's root canvas -- the always-on gameplay HUD container that
        // holds the compass. FindAllOf also returns the blueprint template (the
        // compass-census trap, see compasses_live), so keep only the live
        // /Engine/Transient instance. Returns its CanvasPanel_Root, or nullptr when
        // the HUD is not up (title screen, loading).
        auto find_player_hud() -> UObject*
        {
            std::vector<UObject*> all;
            UObjectGlobals::FindAllOf(STR("WBP_PlayerUI_C"), all);
            UObject* fallback = nullptr;
            for (auto* ui : all)
            {
                if (!ui || ui->GetFullName().find(STR("/Engine/Transient")) == std::wstring::npos)
                {
                    continue;
                }
                auto** root = ui->GetValuePtrByPropertyNameInChain<UObject*>(STR("CanvasPanel_Root"));
                if (!root || !*root)
                {
                    continue;
                }
                // Prefer the VISIBLE HUD. On a server the game tears down and rebuilds
                // WBP_PlayerUI (respawn / world transition); the old instance lingers, and
                // returning that orphan left the compass parented to a dead widget so its
                // markers silently vanished mid-session (Kenny). If none report visible
                // (e.g. while the world map is open the HUD is hidden), fall back to the
                // first found so the cached canvas is kept rather than dropped.
                Engine::ParamsIsVisible vis{};
                if (Engine::call(ui, L"IsVisible", vis) && vis.ReturnValue)
                {
                    return *root;
                }
                if (!fallback)
                {
                    fallback = *root;
                }
            }
            return fallback;
        }

        // Minimap (#26) SPIKE: prove a persistent widget can live on the gameplay
        // HUD. Draws only the frame -- a dark rounded square top-right with a gold
        // "you" dot at centre -- on WBP_PlayerUI's root canvas. The coordinate
        // transform and layer dots come after this is verified in-game. Same
        // StaticConstructObject + AddChildToCanvas machinery as the map overlay,
        // just parented to a canvas that exists during play rather than the map
        // body's. Lifetime keyed on the HUD root's name like build_panel.
        // Remove any of OUR own canvases left on a POOLED/reused HUD root by a prior open.
        // Palworld reuses WBP_PlayerUI instances, and the minimap/compass rebuild path nulls our
        // canvas pointer WITHOUT detaching the old widget (the comment there assumed the old HUD
        // died -- false for a pooled one) -- so a reused HUD would otherwise carry a 2nd (3rd...)
        // canvas and draw every dot twice (Kenny: a "ghost" doubled effigy on the minimap, gone
        // after a map open/close rebuilds the HUD). Only ever touches the current LIVE root, so
        // it is dangling-safe. Prefix-matched because StaticConstructObject may suffix the name
        // ("_1") while a just-swept canvas still holds it pending GC. Mirrors ensure_layer_canvas.
        auto sweep_stale_hud_canvas(UObject* root, const std::wstring& prefix) -> void
        {
            std::vector<UObject*> stale;
            const int32_t n = Engine::children_count(root);
            for (int32_t i = 0; i < n; ++i)
            {
                UObject* c = Engine::child_at(root, i);
                if (c && Engine::widget_name(c).starts_with(prefix))
                {
                    stale.push_back(c);
                }
            }
            for (UObject* s : stale)
            {
                Engine::ParamsRemoveChild rc{s, false};
                Engine::call(root, L"RemoveChild", rc);
            }
            if (!stale.empty())
            {
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] swept {} stale '{}' canvas(es) off a reused HUD\n"), stale.size(),
                    prefix.c_str());
            }
        }

        auto build_minimap() -> void
        {
            if (!g_minimap)
            {
                if (m_minimap_canvas)   // toggled off (F8): hide, keep widgets for re-show
                {
                    Engine::ParamsSetVisibility v{Engine::Vis_Collapsed};
                    Engine::call(m_minimap_canvas, L"SetVisibility", v);
                }
                return;
            }
            // find_player_hud is a FindAllOf (walks the whole object array). Once the
            // minimap is built, only re-scan at ~2 Hz to catch a HUD rebuild -- the
            // per-frame dot/arrow work uses the cached m_minimap_canvas. This is the
            // difference between us and PalMiniMap, which caches its refs and never
            // scans; the per-frame scan was the ~10 FPS cost.
            const auto now = std::chrono::steady_clock::now();
            if (m_minimap_canvas && now < m_next_hud_scan)
            {
                return;
            }
            m_next_hud_scan = now + std::chrono::milliseconds(5000);   // ~23ms FindAllOf, so keep it rare
            UObject* root = find_player_hud();
            if (!root)
            {
                return;
            }
            const std::wstring root_name = root->GetFullName();
            if (m_minimap_canvas)
            {
                if (m_minimap_root_name == root_name)
                {
                    // already built; ensure visible (a prior F8-off may have hidden it)
                    Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                    Engine::call(m_minimap_canvas, L"SetVisibility", v);
                    return;
                }
                m_minimap_canvas = nullptr;   // HUD rebuilt; old canvas died with it
                m_minimap_frame = nullptr;
                m_minimap_bg = nullptr;
                m_minimap_you = nullptr;
                m_you_arrowed = false;
                m_mm_have_last = false;       // force a full redraw into the rebuilt HUD
                m_mm_pc = nullptr;            // world may have changed; re-resolve the PC
                m_minimap_dots.clear();       // pooled dots died with it too
                m_minimap_dot_slots.clear();
                m_minimap_dot_icon.clear();
                // A HUD rebuild often means the world changed, which GC's our capture
                // actor. Drop the stale pointers and let init_terrain re-spawn rather
                // than teleport a dangling actor.
                m_terrain_tried = false;
                m_terrain_actor = nullptr;
                m_terrain_comp = nullptr;
                m_terrain_rt = nullptr;
            }
            auto* canvas_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            if (!canvas_class || !image_class)
            {
                return;
            }
            sweep_stale_hud_canvas(root, L"Lodestone_Minimap");   // drop a leak from a reused HUD
            {
                FStaticConstructObjectParameters params{canvas_class, root};
                params.Name = FName(STR("Lodestone_Minimap"));
                m_minimap_canvas = UObjectGlobals::StaticConstructObject(params);
            }
            if (!m_minimap_canvas)
            {
                return;
            }
            Engine::ParamsAddChildToCanvas add{m_minimap_canvas, nullptr};
            if (!Engine::call(root, L"AddChildToCanvas", add) || !add.ReturnValue)
            {
                m_minimap_canvas = nullptr;
                return;
            }
            UObject* slot = add.ReturnValue;
            // 3x3-grid placement (g_minimap_ax/ay), fixed size. Point anchor =>
            // Offsets = {X, Y, Width, Height}; alignment matches the anchor so the
            // margin is measured from that edge and holds at any resolution. A
            // centered axis (0.5) takes a 0 offset -- the margin only makes sense
            // when pushing in from an edge.
            const double ax = g_minimap_ax;
            const double ay = g_minimap_ay;
            const float ox = (ax == 0.0) ? static_cast<float>(g_minimap_ox)
                             : (ax == 1.0) ? static_cast<float>(-g_minimap_ox) : 0.0f;
            const float oy = (ay == 0.0) ? static_cast<float>(g_minimap_oy)
                             : (ay == 1.0) ? static_cast<float>(-g_minimap_oy) : 0.0f;
            Engine::ParamsSetAnchors anch{ax, ay, ax, ay};
            Engine::call(slot, L"SetAnchors", anch);
            Engine::ParamsSetAlignment align{{ax, ay}};
            Engine::call(slot, L"SetAlignment", align);
            Engine::ParamsSetOffsets offs{ox, oy, static_cast<float>(g_minimap_px),
                                          static_cast<float>(g_minimap_px)};
            Engine::call(slot, L"SetOffsets", offs);
            Engine::ParamsSetZOrder z{50};
            Engine::call(slot, L"SetZOrder", z);

            auto add_child = [&](UObject* w) -> UObject* {
                Engine::ParamsAddChildToCanvas a{w, nullptr};
                return (Engine::call(m_minimap_canvas, L"AddChildToCanvas", a) && a.ReturnValue) ? a.ReturnValue
                                                                                                : nullptr;
            };
            // Frame: a gold rounded square filling the container, drawn first (behind).
            // The dark background sits ~3 px inside it, so the gold reads as a border
            // ring -- always visible, even when terrain is blank or off.
            {
                FStaticConstructObjectParameters params{image_class, m_minimap_canvas};
                if (UObject* frame = UObjectGlobals::StaticConstructObject(params))
                {
                    Style::make_round(frame);
                    Engine::ParamsSetColorAndOpacity c{{Style::kAccentR, Style::kAccentG, Style::kAccentB, 0.95f}};
                    Engine::call(frame, L"SetColorAndOpacity", c);
                    Engine::ParamsSetVisibility sv{Engine::Vis_HitTestInvisible};
                    Engine::call(frame, L"SetVisibility", sv);
                    if (UObject* fslot = add_child(frame))
                    {
                        Engine::ParamsSetAnchors fa{0, 0, 1, 1};
                        Engine::call(fslot, L"SetAnchors", fa);
                        Engine::ParamsSetOffsets fo{0.0f, 0.0f, 0.0f, 0.0f};
                        Engine::call(fslot, L"SetOffsets", fo);
                    }
                    m_minimap_frame = frame;
                }
            }
            // Background: dark rounded square, inset 3 px so the frame shows as a border.
            {
                FStaticConstructObjectParameters params{image_class, m_minimap_canvas};
                if (UObject* bg = UObjectGlobals::StaticConstructObject(params))
                {
                    Style::make_round(bg);
                    Engine::ParamsSetColorAndOpacity c{{0.04f, 0.05f, 0.07f, 0.90f}};
                    Engine::call(bg, L"SetColorAndOpacity", c);
                    Engine::ParamsSetVisibility sv{Engine::Vis_HitTestInvisible};
                    Engine::call(bg, L"SetVisibility", sv);
                    if (UObject* bslot = add_child(bg))
                    {
                        // stretch anchor => Offsets = {Left, Top, Right, Bottom} margins
                        Engine::ParamsSetAnchors fa{0, 0, 1, 1};
                        Engine::call(bslot, L"SetAnchors", fa);
                        Engine::ParamsSetOffsets fo{3.0f, 3.0f, 3.0f, 3.0f};
                        Engine::call(bslot, L"SetOffsets", fo);
                    }
                    m_minimap_bg = bg;
                    // Live terrain (its world actor survives HUD rebuilds) fills the bg.
                    // Wrap the RT in M_CapturedMaterial -- a raw RT brush renders yellow.
                    if (m_terrain_rt)
                    {
                        if (!wrap_rt_in_material(bg, m_terrain_rt))
                        {
                            Engine::ParamsSetBrushResourceObject tb{m_terrain_rt};
                            Engine::call(bg, L"SetBrushResourceObject", tb);
                        }
                        Style::make_image(bg, g_minimap_px);
                        Engine::ParamsSetColorAndOpacity w{{1.0f, 1.0f, 1.0f, 1.0f}};
                        Engine::call(bg, L"SetColorAndOpacity", w);
                    }
                }
            }
            // "You": centre marker. Built as a gold dot so it is visible immediately;
            // tick_minimap upgrades it to the vanilla player arrow (T_icon_map_player)
            // once icons have loaded and rotates it to the player's facing.
            {
                FStaticConstructObjectParameters params{image_class, m_minimap_canvas};
                if (UObject* me = UObjectGlobals::StaticConstructObject(params))
                {
                    Style::make_round(me);
                    Engine::ParamsSetColorAndOpacity c{{1.0f, 0.82f, 0.15f, 1.0f}};
                    Engine::call(me, L"SetColorAndOpacity", c);
                    Engine::ParamsSetVisibility sv{Engine::Vis_HitTestInvisible};
                    Engine::call(me, L"SetVisibility", sv);
                    if (UObject* mslot = add_child(me))
                    {
                        Engine::ParamsSetAnchors ca{0.5, 0.5, 0.5, 0.5};
                        Engine::call(mslot, L"SetAnchors", ca);
                        Engine::ParamsSetAlignment cal{{0.5, 0.5}};
                        Engine::call(mslot, L"SetAlignment", cal);
                        Engine::ParamsSetOffsets co{0.0f, 0.0f, 22.0f, 22.0f};
                        Engine::call(mslot, L"SetOffsets", co);
                        m_minimap_you = me;
                    }
                }
            }
            m_minimap_root_name = root_name;
            if (!m_minimap_logged)
            {
                Output::send<LogLevel::Default>(STR("[Lodestone] minimap frame built on {}\n"), root_name);
                m_minimap_logged = true;
            }
        }

        // Verify a ProcessEvent param struct's offsets against the reflected function
        // before calling it. ProcessEvent copies the struct raw, so a wrong layout
        // corrupts the frame silently instead of failing -- the mod already paid for
        // that lesson on OpenLevel. Logs and returns false on any mismatch.
        auto verify_params(UObject* obj, const wchar_t* fn_name,
                           std::initializer_list<std::pair<const wchar_t*, int32_t>> expect) -> bool
        {
            if (!obj)
            {
                return false;
            }
            UFunction* fn = obj->GetFunctionByNameInChain(FName(fn_name, FNAME_Find));
            if (!fn)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] terrain: {} not found\n"),
                                                std::wstring(fn_name));
                return false;
            }
            for (const auto& [name, want] : expect)
            {
                int32_t got = -1;
                for (FProperty* p : fn->ForEachProperty())
                {
                    if (p->GetName() == name)
                    {
                        got = p->GetOffset_Internal();
                        break;
                    }
                }
                if (got != want)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[Lodestone] terrain: {}::{} @ {:#x}, expected {:#x} -- NOT calling\n"),
                        std::wstring(fn_name), std::wstring(name), got, want);
                    return false;
                }
            }
            return true;
        }

        // capture camera height now lives in g_terrain_height (sweepable via settings)

        // One-shot: create the render target, spawn a top-down SceneCapture2D, point
        // it at the render target, and show that on the minimap background. Every
        // ProcessEvent path is offset-verified first. Gated behind g_minimap_terrain.
        //
        // SHELVED 2026-07-18 with a PRECISE finding (was "flat yellow, needs ShowFlags").
        // The yellow is NOT the capture. Round-2/3 ruled the capture out: BaseColor (no
        // fog/lighting) is still yellow; readback confirms Ortho + renderMode=1 +
        // maxview=-1 + bAlwaysPersistRenderingState + a forced CaptureScene() all applied;
        // the actor logs pitch=-90 (looking straight down). The decisive test: clearing
        // the RT to MAGENTA left the minimap bg YELLOW -- so the widget never shows our RT
        // at all. Showing a UTextureRenderTarget2D directly via UImage::SetBrushResourceObject
        // does not display on this build (yellow is the broken-RT fallback). This is why
        // PalMiniMap wraps its RT in a MATERIAL. NEXT ATTEMPT: a UMaterialInstanceDynamic
        // over a base material with a texture parameter (a UI TextureSample material --
        // Palworld's character-preview UI samples a capture RT, a candidate to reuse) ->
        // SetTextureParameterValue(param, RT) -> SetBrushResourceObject(the material).
        // Wrap a render target in the vanilla M_CapturedMaterial and set it as `image`'s
        // brush, so the RT actually displays (a raw RT brush renders flat yellow here).
        // Returns false if the material or any reflected step is unavailable -- the caller
        // then falls back to the raw-RT brush.
        auto wrap_rt_in_material(UObject* image, UObject* rt) -> bool
        {
            if (!image || !rt)
            {
                return false;
            }
            if (!m_captured_material)
            {
                m_captured_material = load_object_by_path(
                    STR("/Game/Pal/Blueprint/UI/SceneCaptureWidget/M_CapturedMaterial.M_CapturedMaterial"));
            }
            if (!m_captured_material)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] terrain: M_CapturedMaterial not found\n"));
                return false;
            }
            // Point the brush at the base material, then mint a dynamic instance to parameterise.
            Engine::ParamsSetBrushFromMaterial sbm{m_captured_material};
            if (!Engine::call(image, L"SetBrushFromMaterial", sbm))
            {
                return false;
            }
            Engine::ParamsGetDynamicMaterial gdm{};
            if (!Engine::call(image, L"GetDynamicMaterial", gdm) || !gdm.ReturnValue)
            {
                return false;
            }
            // Feed the render target into the material's "RenderTarget" texture parameter.
            if (!verify_params(gdm.ReturnValue, L"SetTextureParameterValue",
                               {{L"ParameterName", 0x00}, {L"Value", 0x08}}))
            {
                return false;
            }
            Engine::ParamsSetTextureParameterValue stp{};
            stp.ParameterName = FName(STR("RenderTarget"), FNAME_Add);
            stp.Value = rt;
            return Engine::call(gdm.ReturnValue, L"SetTextureParameterValue", stp);
        }

        auto init_terrain(double px, double py, double pz) -> void
        {
            m_terrain_tried = true;
            auto* krl = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
            auto* gps = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            auto* cap_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.SceneCapture2D"));
            auto* pc = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (!krl || !gps || !cap_class || !pc)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] terrain: missing CDO/class/controller\n"));
                return;
            }

            // 1) render target
            if (!verify_params(krl, L"CreateRenderTarget2D",
                               {{L"WorldContextObject", 0x00}, {L"Width", 0x08}, {L"Height", 0x0C},
                                {L"Format", 0x10}, {L"ClearColor", 0x14}, {L"ReturnValue", 0x28}}))
            {
                return;
            }
            Engine::ParamsCreateRenderTarget2D crt{};
            crt.WorldContextObject = pc;
            crt.Width = 512;
            crt.Height = 512;
            crt.Format = 2;   // RTF_RGBA8
            crt.ClearColor = {0.0f, 0.0f, 0.0f, 1.0f};
            Engine::call(krl, L"CreateRenderTarget2D", crt);
            if (!crt.ReturnValue)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] terrain: CreateRenderTarget2D returned null\n"));
                return;
            }
            m_terrain_rt = crt.ReturnValue;

            // 2) spawn the capture actor (deferred so we can configure before it runs)
            if (!verify_params(gps, L"BeginDeferredActorSpawnFromClass",
                               {{L"WorldContextObject", 0x00}, {L"actorClass", 0x08},
                                {L"SpawnTransform", 0x10}, {L"collisionHandlingOverride", 0x70},
                                {L"Owner", 0x78}, {L"ReturnValue", 0x80}}) ||
                !verify_params(gps, L"FinishSpawningActor",
                               {{L"Actor", 0x00}, {L"SpawnTransform", 0x10}, {L"ReturnValue", 0x70}}))
            {
                m_terrain_rt = nullptr;
                return;
            }
            Engine::FTransform_ xf{};
            xf.TX = px;
            xf.TY = py;
            xf.TZ = pz + g_terrain_height;
            Engine::ParamsBeginDeferredActorSpawn beg{};
            beg.WorldContextObject = pc;
            beg.ActorClass = cap_class;
            beg.SpawnTransform = xf;
            beg.CollisionHandlingOverride = 1;   // AlwaysSpawn
            Engine::call(gps, L"BeginDeferredActorSpawnFromClass", beg);
            if (!beg.ReturnValue)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] terrain: spawn returned null\n"));
                m_terrain_rt = nullptr;
                return;
            }
            m_terrain_actor = beg.ReturnValue;

            // 3) configure the capture component (direct property writes, no ProcessEvent)
            if (auto** comp = m_terrain_actor->GetValuePtrByPropertyNameInChain<UObject*>(STR("CaptureComponent2D"));
                comp && *comp)
            {
                m_terrain_comp = *comp;
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(STR("ProjectionType")))
                    *v = 1;   // Orthographic
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<float>(STR("OrthoWidth")))
                    *v = static_cast<float>(2.0 * g_minimap_range_uu);
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<UObject*>(STR("TextureTarget")))
                    *v = m_terrain_rt;
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CaptureSource")))
                    *v = static_cast<uint8_t>(g_terrain_source);
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(STR("bCaptureEveryFrame")))
                    *v = 1;
                // Keep the capture's rendering state alive between frames. Without this a
                // capture frequently renders an EMPTY scene (flat colour) -- the classic
                // cause of a SceneCapture that "does nothing". Prime suspect for the yellow,
                // since even BaseColor (no fog/lighting) came back flat.
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(STR("bAlwaysPersistRenderingState")))
                    *v = 1;
                // Render ALL scene primitives (PRM_RenderScenePrimitives=1). A wrong
                // default of PRM_UseShowOnlyList with an empty ShowOnlyActors would render
                // nothing but sky -> a flat colour, one candidate for the yellow.
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(STR("PrimitiveRenderMode")))
                    *v = 1;
                // Unlimited view distance so the ground below is never far-clipped.
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<float>(STR("MaxViewDistanceOverride")))
                    *v = -1.0f;
                m_terrain_ortho = 2.0 * g_minimap_range_uu;
                // Exposure = EV bias only, method LEFT AT the capture default. Forcing
                // AEM_Manual (method=2) blacked the capture out entirely (Kenny: "minimap
                // went black") -- manual exposure derives brightness from camera EV100 with
                // no metering, and this capture's base sits near zero, so a bias of a few
                // stops can't lift it. The default (auto) method renders the terrain (the
                // visible blue relief), so keep it and only nudge the bias.
                // FPostProcessSettings (this build, offsets from CXXHeaderDump):
                //   bOverride_AutoExposureBias = bit5 (0x20) @0x0009
                //   AutoExposureBias (EV stops)             @0x047C
                if (auto* pp = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(STR("PostProcessSettings")))
                {
                    pp[0x0009] |= 0x20;   // bOverride_AutoExposureBias
                    *reinterpret_cast<float*>(pp + 0x047C) = static_cast<float>(g_terrain_exposure);
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] terrain exposure: method={} (default) bias={:.2f}\n"),
                        static_cast<int>(pp[0x002A]), *reinterpret_cast<float*>(pp + 0x047C));
                }
                // Read the values back so the log proves what actually stuck vs a silently
                // ignored write, and dump a VANILLA capture (menu/Pal preview) to diff.
                auto rd8 = [&](const wchar_t* n) -> int {
                    auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<uint8_t>(n);
                    return v ? static_cast<int>(*v) : -1;
                };
                auto rdf = [&](const wchar_t* n) -> float {
                    auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<float>(n);
                    return v ? *v : -999.0f;
                };
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] terrain readback: source={} proj={} everyFrame={} renderMode={} "
                        "ortho={:.0f} maxview={:.0f} height={:.0f}\n"),
                    rd8(L"CaptureSource"), rd8(L"ProjectionType"), rd8(L"bCaptureEveryFrame"),
                    rd8(L"PrimitiveRenderMode"), rdf(L"OrthoWidth"), rdf(L"MaxViewDistanceOverride"),
                    g_terrain_height);
                std::vector<UObject*> caps;
                UObjectGlobals::FindAllOf(STR("SceneCaptureComponent2D"), caps);
                int shown = 0;
                for (auto* c : caps)
                {
                    if (!c || c == m_terrain_comp)
                    {
                        continue;
                    }
                    auto rv = [&](const wchar_t* n) -> int {
                        auto* v = c->GetValuePtrByPropertyNameInChain<uint8_t>(n);
                        return v ? static_cast<int>(*v) : -1;
                    };
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] terrain: vanilla capture source={} proj={} renderMode={} everyFrame={} {}\n"),
                        rv(L"CaptureSource"), rv(L"ProjectionType"), rv(L"PrimitiveRenderMode"),
                        rv(L"bCaptureEveryFrame"), c->GetFullName());
                    if (++shown >= 3)
                    {
                        break;
                    }
                }
            }

            // 4) finish the deferred spawn
            Engine::ParamsFinishSpawningActor fin{};
            fin.Actor = m_terrain_actor;
            fin.SpawnTransform = xf;
            Engine::call(gps, L"FinishSpawningActor", fin);

            // 5) show the render target on the minimap background, wrapped in a material.
            // A raw RT via SetBrushResourceObject renders flat yellow on this build; the
            // vanilla M_CapturedMaterial (UI material whose texture param is "RenderTarget")
            // displays it correctly -- the same trick PalMiniMap uses. make_image flips the
            // brush draw type RoundedBox->Image without touching the material resource.
            if (m_minimap_bg)
            {
                bool wrapped = wrap_rt_in_material(m_minimap_bg, m_terrain_rt);
                if (!wrapped)
                {
                    // Fallback: raw RT (yellow, but proves the capture path is alive).
                    Engine::ParamsSetBrushResourceObject tb{m_terrain_rt};
                    Engine::call(m_minimap_bg, L"SetBrushResourceObject", tb);
                }
                Style::make_image(m_minimap_bg, g_minimap_px);
                Engine::ParamsSetColorAndOpacity w{{1.0f, 1.0f, 1.0f, 1.0f}};
                Engine::call(m_minimap_bg, L"SetColorAndOpacity", w);
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] terrain: RT on bg (material-wrapped={})\n"), wrapped ? 1 : 0);
            }
            Output::send<LogLevel::Default>(STR("[Lodestone] terrain: SceneCapture2D spawned + wired\n"));
        }

        // Strip the dynamic scene -- pals, players, NPCs, floating UI icons (the palbox
        // diamond), base beacons -- out of the terrain CAPTURE only, so the minimap reads
        // like a clean map instead of a top-down photo. HideActorComponents/HideComponent
        // touch this capture, not the main view. PalMiniMap's clean look = terrain capture
        // + icon overlays; this gets us the terrain half. Round-robin ONE FindAllOf per
        // fire (each ~20 ms -- see the minimap perf note) so there is no periodic stutter;
        // each class is re-swept ~every 2.4 s, fine since these stream in slowly. The
        // hidden lists AddUnique, so re-firing does not grow them unbounded.
        auto clean_terrain_capture() -> void
        {
            if (!m_terrain_comp || !g_minimap_terrain)
            {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < m_next_capture_clean)
            {
                return;
            }
            m_next_capture_clean = now + std::chrono::milliseconds(800);

            std::vector<UObject*> found;
            const wchar_t* what = L"";
            if (m_capture_clean_idx == 0)
            {
                UObjectGlobals::FindAllOf(STR("PalCharacter"), found);
                what = L"pals";
                for (auto* a : found)
                {
                    if (a)
                    {
                        Engine::ParamsHideActorComponents p{a, true};
                        Engine::call(m_terrain_comp, L"HideActorComponents", p);
                    }
                }
            }
            else if (m_capture_clean_idx == 1)
            {
                UObjectGlobals::FindAllOf(STR("WidgetComponent"), found);
                what = L"widgets";
                for (auto* c : found)
                {
                    if (c)
                    {
                        Engine::ParamsHideComponent p{c};
                        Engine::call(m_terrain_comp, L"HideComponent", p);
                    }
                }
            }
            else
            {
                UObjectGlobals::FindAllOf(STR("MaterialBillboardComponent"), found);
                what = L"billboards";
                for (auto* c : found)
                {
                    if (c)
                    {
                        Engine::ParamsHideComponent p{c};
                        Engine::call(m_terrain_comp, L"HideComponent", p);
                    }
                }
            }
            m_capture_clean_idx = (m_capture_clean_idx + 1) % 3;
            static int logn = 0;
            if (logn < 12)
            {
                ++logn;
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] terrain clean: hid {} {} from capture\n"),
                    static_cast<int>(found.size()), what);
            }
        }

        // Per-tick: keep the capture straight above the player, looking down, and
        // track zoom changes into OrthoWidth. Verified once in init; K2_TeleportTo has
        // no out-param so it is safe to call without re-verifying each frame.
        auto tick_terrain(double px, double py, double pz, double cam_yaw) -> void
        {
            if (!m_terrain_actor)
            {
                return;
            }
            Engine::ParamsTeleportTo tp{};
            tp.DestLocation = {px, py, pz + g_terrain_height};
            tp.DestRotation = {-90.0, cam_yaw, 0.0};   // pitch down, yaw spins the capture
            Engine::call(m_terrain_actor, L"K2_TeleportTo", tp);
            if (m_terrain_comp && m_terrain_ortho != 2.0 * g_minimap_range_uu)
            {
                if (auto* v = m_terrain_comp->GetValuePtrByPropertyNameInChain<float>(STR("OrthoWidth")))
                    *v = static_cast<float>(2.0 * g_minimap_range_uu);
                m_terrain_ortho = 2.0 * g_minimap_range_uu;
            }
            if (m_terrain_comp)
            {
                // Force a capture AFTER this frame's look-down rotation is applied -- in
                // case bCaptureEveryFrame snapshots at the stale spawn rotation (identity =
                // horizontal), which would explain an empty/flat capture.
                struct
                {
                } noargs;
                Engine::call(m_terrain_comp, L"CaptureScene", noargs);
            }
            // One-shot: confirm the actor really points down (pitch ~ -90) a few frames in.
            if (m_terrain_rot_log < 3)
            {
                ++m_terrain_rot_log;
                Engine::ParamsGetActorRotation gr{};
                if (Engine::call(m_terrain_actor, L"K2_GetActorRotation", gr))
                {
                    Output::send<LogLevel::Default>(
                        STR("[Lodestone] terrain: capture actor rot pitch={:.0f} yaw={:.0f} roll={:.0f}\n"),
                        gr.ReturnValue.Pitch, gr.ReturnValue.Yaw, gr.ReturnValue.Roll);
                }
            }
        }

        // Apply the minimap hotkey requests (set on the input thread) on the game
        // thread. Called from whichever loop drives the minimap.
        auto handle_minimap_input() -> void
        {
            if (m_minimap_toggle_req.exchange(false))   // F8
            {
                g_minimap = !g_minimap;
                save_settings();
            }
            if (int z = m_minimap_zoom_req.exchange(0); z != 0)   // F9/F10, 1.25x per step
            {
                // Floor at 4000 uu (40 m radius). The terrain is a LIVE SceneCapture, so
                // real 3-D pals near the player render into it; zooming in shrinks OrthoWidth
                // and those pals balloon. 40 m keeps them small (unlike PalMiniMap, which
                // captures terrain only + draws pal ICONS, our capture is the whole scene).
                g_minimap_range_uu = std::clamp(g_minimap_range_uu * std::pow(1.25, z), 4000.0, 150000.0);
                save_settings();
            }
            if (m_minimap_rotate_req.exchange(false))   // F6
            {
                g_minimap_rotate = !g_minimap_rotate;
                save_settings();
            }
            if (m_compass_toggle_req.exchange(false))   // F7
            {
                g_compass = !g_compass;
                save_settings();
            }
            if (m_census_req.exchange(false))   // F4 (dev): dump the spawner tables
            {
                census_spawners();
            }
        }

        // Access-violation filter: swallow AVs (a dangling UObject touched during world
        // teardown), let everything else -- including C++ exceptions -- propagate.
        static auto minimap_av_filter(unsigned int code) -> int
        {
            return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
        }

        // SEH layer of the per-frame minimap driver. An access violation here means we
        // touched a freed object as the world tore down (the crash on leaving a server)
        // -- skip the frame instead of crashing the game. MSVC forbids __try in a
        // function with C++ objects needing unwind, so this scope has none; the work is
        // in the called functions, and a real C++ throw (ProcessEvent not available)
        // passes through to minimap_frame's catch.
        auto minimap_frame_seh() -> void
        {
            __try
            {
                handle_minimap_input();
                // Live nearest scan for the actor layers (eggs/dungeons/lucky) + lucky range
                // alert. Self-throttled round-robin (one FindAllOf per fire), ungated by the
                // compass toggle so the alert fires while walking; the compass tick injects the
                // results into m_nearest so eggs that spawn nearby show WITHOUT a map open.
                refresh_live_nearest();
                // Compass FIRST. It is the primary nav tool and default-on; the minimap is
                // default-off and the more teardown-fragile of the two (SceneCapture actor,
                // dense dot pool). Running the compass ahead of the minimap means a
                // minimap-side AV can't starve it via the shared __except below -- on a
                // server the strip had been dying because a fault after tick_minimap skipped
                // build_compass/tick_compass_strip every frame.
                build_compass();
                tick_compass_strip();
                // Time build vs tick separately to see which carries the residual cost
                // (periodic FindAllOf in build, or per-frame work in tick). Only trivial
                // locals here -- the MSVC __try rule forbids objects needing unwind; the
                // Output::send lives in minimap_frame() below.
                const auto t0 = std::chrono::steady_clock::now();
                build_minimap();
                const auto t1 = std::chrono::steady_clock::now();
                tick_minimap();
                const auto t2 = std::chrono::steady_clock::now();
                const long long bus = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                const long long tus = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
                if (bus > m_mm_build_max)
                {
                    m_mm_build_max = bus;
                }
                if (tus > m_mm_tick_max)
                {
                    m_mm_tick_max = tus;
                }
                // Heartbeat: stamp ONLY after the whole body completes cleanly. If any call
                // above AV'd (server world teardown), the __except skips this and
                // m_last_frame_hook goes stale within ~1 s -> tick_map's fallback takes over
                // driving input + compass. Stamping at hook ENTRY (the old spot) made a
                // fires-but-faults hook look alive forever, suppressing the fallback and
                // freezing the compass on a server -- F7 was never consumed (Kenny).
                m_last_frame_hook = std::chrono::steady_clock::now();
            }
            __except (minimap_av_filter(GetExceptionCode()))
            {
            }
        }

        // C++ layer: a reflected call can throw "ProcessEvent not available" during a
        // transition, and UE4SS removes a hook for good on any escaping exception -- so
        // catch it and keep the per-frame hook alive.
        auto minimap_frame() -> void
        {
            // The heartbeat (m_last_frame_hook) is stamped inside minimap_frame_seh AFTER the
            // body completes, NOT here at entry -- so a hook that fires but faults every frame
            // reads as stalled and tick_map's fallback rescues the compass (the server bug).
            try
            {
                minimap_frame_seh();
                // Report the worst build/tick cost each ~5 s window (string work lives
                // here, outside the __try). First window is skipped (maxes not seeded).
                const auto now = std::chrono::steady_clock::now();
                if (g_minimap && now >= m_mm_next_log)
                {
                    if (m_mm_next_log.time_since_epoch().count() != 0)
                    {
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] minimap perf: build max {}us, tick max {}us (5s)\n"),
                            m_mm_build_max, m_mm_tick_max);
                    }
                    m_mm_next_log = now + std::chrono::seconds(5);
                    m_mm_build_max = 0;
                    m_mm_tick_max = 0;
                }
            }
            catch (...)
            {
            }
        }

        // Fill the minimap with your toggled-on layers as dots placed relative to the
        // player. Two orientations: ROTATE (player-forward = up, the dot field spins by
        // your yaw, "you" arrow fixed up) or NORTH-UP (map fixed, arrow rotates). The
        // arrow + terrain-follow run every call so they are smooth; the dot relayout,
        // which walks every on-layer's points, is throttled. World +X = east, +Y =
        // north; screen +y is down, hence the sy flip.
        auto tick_minimap() -> void
        {
            if (!g_minimap || !m_minimap_canvas)
            {
                return;
            }
            double px = 0, py = 0, pz = 0, yaw = 0;
            bool has_yaw = false;
            if (!player_pose(px, py, pz, yaw, has_yaw))   // one PC+pawn walk, not two
            {
                return;
            }
            const bool rotate = g_minimap_rotate && has_yaw;

            // Idle-skip: standing still needs no minimap update -- otherwise the
            // per-frame arrow SetRenderTransformAngle invalidates Slate every frame,
            // the residual "hit when the minimap is up". Compare against the last pose
            // we actually DREW (not the previous frame) so slow drift still accumulates
            // to an update; any real walking/turning exceeds the epsilon at once.
            const bool idle = m_mm_have_last && std::abs(px - m_mm_last_px) < 2.0 &&
                              std::abs(py - m_mm_last_py) < 2.0 &&
                              (!has_yaw || std::abs(yaw - m_mm_last_yaw) < 0.3);

            if (g_minimap_terrain)   // opt-in live terrain capture behind the dots
            {
                if (!m_terrain_tried)
                {
                    init_terrain(px, py, pz);
                }
                // Rotate the capture with the player so the ground lines up with the
                // spun dot field; fixed north in north-up mode.
                tick_terrain(px, py, pz, rotate ? yaw : 0.0);
            }

            // "You" marker: upgrade the gold dot to the vanilla player arrow once icons
            // load. Rotate mode keeps it pointing up (you always face up); north-up it
            // rotates to your facing (90 - yaw, verified in-game).
            if (m_minimap_you)
            {
                bool just_arrowed = false;
                if (!m_you_arrowed && m_icons_ready.load(std::memory_order_acquire))
                {
                    if (UObject* tex = layer_texture(L"T_icon_map_player"))
                    {
                        Engine::ParamsSetBrushFromTexture b{tex, false};
                        Engine::call(m_minimap_you, L"SetBrushFromTexture", b);
                        Style::make_image(m_minimap_you, 22.0);
                        Engine::ParamsSetColorAndOpacity w{{1.0f, 1.0f, 1.0f, 1.0f}};
                        Engine::call(m_minimap_you, L"SetColorAndOpacity", w);
                        m_you_arrowed = true;
                        just_arrowed = true;
                    }
                }
                // Only re-rotate when the facing actually changed (or the arrow was just
                // applied) -- skip the Slate invalidation while idle.
                if (has_yaw && (!idle || just_arrowed))
                {
                    const float ang = rotate ? 0.0f : static_cast<float>(90.0 - yaw);
                    Engine::ParamsSetRenderTransformAngle a{ang};
                    Engine::call(m_minimap_you, L"SetRenderTransformAngle", a);
                }
            }

            if (idle)
            {
                return;   // standing still: skip the dot relayout entirely
            }
            m_mm_last_px = px;
            m_mm_last_py = py;
            if (has_yaw)
            {
                m_mm_last_yaw = yaw;
            }
            m_mm_have_last = true;

            // The dot relayout is the expensive part (walks every on-layer's points) --
            // throttle it; the smooth bits above already ran.
            const auto now = std::chrono::steady_clock::now();
            if (now < m_next_minimap)
            {
                return;
            }
            m_next_minimap = now + std::chrono::milliseconds(100);
            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            if (!image_class)
            {
                return;
            }
            const double half = g_minimap_px / 2.0;
            const double scale = half / g_minimap_range_uu;
            const bool icons = m_icons_ready.load(std::memory_order_acquire);
            // Rotate mode: turn each offset by (90 + yaw) so player-forward maps to +Y
            // (screen up). The yaw sign matches the compass fix -- the game's yaw is
            // clockwise, so (90 - yaw) spun the field the wrong way (Kenny: "rotated the
            // wrong direction"). The north-up arrow keeps (90 - yaw): that render-angle
            // path is confirmed correct (SW rock -> SW arrow) and is separate geometry.
            const double a = rotate ? (90.0 + yaw) * 3.14159265358979323846 / 180.0 : 0.0;
            const double ca = std::cos(a), saa = std::sin(a);

            size_t cursor = 0;
            auto place = [&](double wx, double wy, const wchar_t* icon, const Engine::FLinearColor_& col) {
                double dx = wx - px, dy = wy - py;
                if (rotate)
                {
                    const double rx = dx * ca - dy * saa;
                    const double ry = dx * saa + dy * ca;
                    dx = rx;
                    dy = ry;
                }
                if (dx < -g_minimap_range_uu || dx > g_minimap_range_uu || dy < -g_minimap_range_uu ||
                    dy > g_minimap_range_uu || cursor >= kMinimapMaxDots)
                {
                    return;
                }
                const float sx = static_cast<float>(half + dx * scale);
                const float sy = static_cast<float>(half - dy * scale);   // +Y world = up
                if (cursor >= m_minimap_dots.size())
                {
                    FStaticConstructObjectParameters params{image_class, m_minimap_canvas};
                    UObject* d = UObjectGlobals::StaticConstructObject(params);
                    if (!d)
                    {
                        return;
                    }
                    Engine::ParamsAddChildToCanvas a{d, nullptr};
                    if (!Engine::call(m_minimap_canvas, L"AddChildToCanvas", a) || !a.ReturnValue)
                    {
                        return;
                    }
                    Engine::ParamsSetAnchors an{0, 0, 0, 0};
                    Engine::call(a.ReturnValue, L"SetAnchors", an);
                    Engine::ParamsSetAlignment al{{0.5, 0.5}};
                    Engine::call(a.ReturnValue, L"SetAlignment", al);
                    m_minimap_dots.push_back(d);
                    m_minimap_dot_slots.push_back(a.ReturnValue);
                    m_minimap_dot_icon.push_back(nullptr);
                }
                UObject* dot = m_minimap_dots[cursor];
                // Only touch the brush when this pooled slot changes what it shows:
                // pooled dots are reused across layers each update but rarely change,
                // and SetBrushFromTexture per dot per tick would be the expensive part.
                if (m_minimap_dot_icon[cursor] != icon)
                {
                    UObject* tex = (icons && icon) ? layer_texture(icon) : nullptr;
                    if (tex)
                    {
                        Engine::ParamsSetBrushFromTexture b{tex, false};
                        Engine::call(dot, L"SetBrushFromTexture", b);
                        Style::make_image(dot, kMinimapDotPx);
                        Engine::ParamsSetColorAndOpacity c{
                            Style::icon_is_glyph(icon) ? col : Engine::FLinearColor_{1.0f, 1.0f, 1.0f, 1.0f}};
                        Engine::call(dot, L"SetColorAndOpacity", c);
                    }
                    else
                    {
                        Style::make_round(dot);   // icons not ready yet: plain coloured dot
                        Engine::ParamsSetColorAndOpacity c{col};
                        Engine::call(dot, L"SetColorAndOpacity", c);
                    }
                    m_minimap_dot_icon[cursor] = tex ? icon : nullptr;
                }
                Engine::ParamsSetOffsets o{sx, sy, static_cast<float>(kMinimapDotPx),
                                           static_cast<float>(kMinimapDotPx)};
                Engine::call(m_minimap_dot_slots[cursor], L"SetOffsets", o);
                Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                Engine::call(dot, L"SetVisibility", v);
                ++cursor;
            };
            int li = 0;
            for (const auto& layer : Data::kLayers)
            {
                const int id = li++;
                if (!is_layer_on(id))
                {
                    continue;
                }
                const Engine::FLinearColor_ col{Style::srgb8(layer.r), Style::srgb8(layer.g),
                                                Style::srgb8(layer.b), 1.0f};
                for (size_t i = 0; i < layer.count; ++i)
                {
                    place(static_cast<double>(layer.points[i].x), static_cast<double>(layer.points[i].y),
                          layer.icon, col);
                }
            }
            // Collectable + live layers (effigies/notes/eggs/dungeons/fast-travel/tower/
            // sealed/lucky) are NOT in kLayers -- they live in the map dot pool (id >= 1000).
            // Draw them from m_dots so the minimap mirrors the big map: collected-aware
            // (base_hidden) and relic-type filtered (the pool was emitted under g_track_relic).
            // Populated after the first map open (like the compass); the static kLayers above
            // already show pre-open. Same per-tick walk cost class as compute_nearest.
            const size_t pooled = std::min(m_emit_cursor, m_dots.size());
            for (size_t i = 0; i < pooled; ++i)
            {
                const Dot& d = m_dots[i];
                if (d.layer_id < 1000 || d.base_hidden || !is_layer_on(d.layer_id))
                {
                    continue;
                }
                place(static_cast<double>(d.wx), static_cast<double>(d.wy), d.icon, d.color);
            }
            for (size_t i = cursor; i < m_minimap_dots.size(); ++i)
            {
                Engine::ParamsSetVisibility v{Engine::Vis_Collapsed};
                Engine::call(m_minimap_dots[i], L"SetVisibility", v);
            }
        }

        // Build the compass strip once on the HUD root (mirrors build_minimap): a dark
        // backing bar + a centre "ahead" tick. Markers are added lazily by
        // tick_compass_strip. Lifetime is keyed on the HUD root name, so a rebuilt HUD
        // (respawn/reload) rebuilds the strip instead of orphaning it.
        auto build_compass() -> void
        {
            if (!g_compass)
            {
                if (m_compass_canvas)   // toggled off (F7): hide, keep widgets for re-show
                {
                    Engine::ParamsSetVisibility v{Engine::Vis_Collapsed};
                    Engine::call(m_compass_canvas, L"SetVisibility", v);
                }
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (m_compass_canvas)
            {
                // Re-show every call (cheap) so F7 off->on restores the strip IMMEDIATELY --
                // the toggle-off above collapsed it, and the scan throttle below used to
                // early-return before the re-show, hiding it for up to 5 s (Kenny).
                Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                Engine::call(m_compass_canvas, L"SetVisibility", v);
            }
            if (m_compass_canvas && now < m_next_compass_scan)
            {
                return;   // built; only re-scan the HUD at ~0.2 Hz to catch a rebuild
            }
            m_next_compass_scan = now + std::chrono::milliseconds(5000);
            UObject* root = find_player_hud();
            if (!root)
            {
                return;
            }
            const std::wstring root_name = root->GetFullName();
            if (m_compass_canvas)
            {
                if (m_compass_root_name == root_name)
                {
                    Engine::ParamsSetVisibility v{Engine::Vis_HitTestInvisible};
                    Engine::call(m_compass_canvas, L"SetVisibility", v);
                    return;
                }
                m_compass_canvas = nullptr;   // HUD rebuilt; old canvas died with it
                m_compass_backing = nullptr;
                m_compass_center = nullptr;
                m_compass_markers.clear();    // pooled markers died with it too
                m_compass_marker_ix.clear();  // layer->marker map points into the dead pool
                m_compass_have_last = false;
            }
            auto* canvas_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            if (!canvas_class || !image_class)
            {
                return;
            }
            sweep_stale_hud_canvas(root, L"Lodestone_Compass");   // same reused-HUD leak as the minimap
            {
                FStaticConstructObjectParameters params{canvas_class, root};
                params.Name = FName(STR("Lodestone_Compass"));
                m_compass_canvas = UObjectGlobals::StaticConstructObject(params);
            }
            if (!m_compass_canvas)
            {
                return;
            }
            Engine::ParamsAddChildToCanvas add{m_compass_canvas, nullptr};
            if (!Engine::call(root, L"AddChildToCanvas", add) || !add.ReturnValue)
            {
                m_compass_canvas = nullptr;
                return;
            }
            UObject* slot = add.ReturnValue;
            const double ax = g_compass_ax, ay = g_compass_ay;
            const float ox = (ax == 0.0) ? static_cast<float>(g_compass_ox)
                             : (ax == 1.0) ? static_cast<float>(-g_compass_ox) : 0.0f;
            const float oy = (ay == 0.0) ? static_cast<float>(g_compass_oy)
                             : (ay == 1.0) ? static_cast<float>(-g_compass_oy) : 0.0f;
            Engine::ParamsSetAnchors anch{ax, ay, ax, ay};
            Engine::call(slot, L"SetAnchors", anch);
            Engine::ParamsSetAlignment align{{ax, ay}};
            Engine::call(slot, L"SetAlignment", align);
            Engine::ParamsSetOffsets offs{ox, oy, static_cast<float>(g_compass_w),
                                          static_cast<float>(g_compass_h)};
            Engine::call(slot, L"SetOffsets", offs);
            Engine::ParamsSetZOrder z{50};
            Engine::call(slot, L"SetZOrder", z);

            auto add_child = [&](UObject* w) -> UObject* {
                Engine::ParamsAddChildToCanvas a{w, nullptr};
                return (Engine::call(m_compass_canvas, L"AddChildToCanvas", a) && a.ReturnValue) ? a.ReturnValue
                                                                                                : nullptr;
            };
            // Dark backing bar so the item icons read against the bright world behind.
            {
                FStaticConstructObjectParameters params{image_class, m_compass_canvas};
                if (UObject* bg = UObjectGlobals::StaticConstructObject(params))
                {
                    Style::make_round(bg, 8.0);
                    Engine::ParamsSetColorAndOpacity c{{0.04f, 0.05f, 0.07f, 0.55f}};
                    Engine::call(bg, L"SetColorAndOpacity", c);
                    Engine::ParamsSetVisibility sv{Engine::Vis_HitTestInvisible};
                    Engine::call(bg, L"SetVisibility", sv);
                    if (UObject* bslot = add_child(bg))
                    {
                        Engine::ParamsSetAnchors fa{0, 0, 1, 1};   // stretch: offsets = margins
                        Engine::call(bslot, L"SetAnchors", fa);
                        Engine::ParamsSetOffsets fo{0.0f, 0.0f, 0.0f, 0.0f};
                        Engine::call(bslot, L"SetOffsets", fo);
                    }
                    m_compass_backing = bg;
                }
            }
            // Centre tick: a thin gold vertical bar marking "straight ahead".
            {
                FStaticConstructObjectParameters params{image_class, m_compass_canvas};
                if (UObject* tick = UObjectGlobals::StaticConstructObject(params))
                {
                    Style::make_round(tick, 1.0);
                    Engine::ParamsSetColorAndOpacity c{{Style::kAccentR, Style::kAccentG, Style::kAccentB, 0.9f}};
                    Engine::call(tick, L"SetColorAndOpacity", c);
                    Engine::ParamsSetVisibility sv{Engine::Vis_HitTestInvisible};
                    Engine::call(tick, L"SetVisibility", sv);
                    if (UObject* tslot = add_child(tick))
                    {
                        Engine::ParamsSetAnchors ca{0.5, 0.5, 0.5, 0.5};
                        Engine::call(tslot, L"SetAnchors", ca);
                        Engine::ParamsSetAlignment cal{{0.5, 0.5}};
                        Engine::call(tslot, L"SetAlignment", cal);
                        Engine::ParamsSetOffsets co{0.0f, 0.0f, 2.0f, static_cast<float>(g_compass_h - 10.0)};
                        Engine::call(tslot, L"SetOffsets", co);
                    }
                    m_compass_center = tick;
                }
            }
            m_compass_root_name = root_name;
            // Log every (re)build, not just the first -- mirrors the minimap, and is how we
            // confirm the strip re-attaches to a server's rebuilt HUD (it only reaches here on
            // a fresh construct, so this is not per-frame spam).
            Output::send<LogLevel::Default>(STR("[Lodestone] compass strip built on {}\n"), root_name);
        }

        // Place the nearest uncollected node of each enabled layer on the strip at its
        // CAMERA-relative bearing. No object-array walk: player_pose uses the cached PC,
        // and the nearest set is the pre-computed m_nearest (refreshed at ~1 Hz here).
        auto tick_compass_strip() -> void
        {
            if (!g_compass)
            {
                return;
            }
            // Rate-limited "why is the strip empty" probe. The strip going silent
            // mid-session was the tick early-returning; this says which gate.
            auto dbg_gate = [this](const wchar_t* why) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= m_next_compass_dbg)
                {
                    m_next_compass_dbg = now + std::chrono::milliseconds(3000);
                    Output::send<LogLevel::Default>(STR("[Lodestone] compass idle: {}\n"), why);
                }
            };
            if (!m_compass_canvas)
            {
                dbg_gate(L"no canvas (HUD not found/orphaned -- build_compass could not attach)");
                return;
            }
            double px = 0, py = 0, pz = 0, yaw = 0;
            bool has_yaw = false;
            if (!player_pose(px, py, pz, yaw, has_yaw) || !has_yaw)
            {
                dbg_gate(L"no camera yaw (player_pose/GetControlRotation failed)");
                return;   // no camera heading -> can't place camera-relative; hold
            }
            const auto now = std::chrono::steady_clock::now();
            // Refresh the nearest-per-layer set at ~1 Hz (a ~7.6k-dot pass). It needs the
            // map opened once so m_dots/m_calibration exist; until then m_nearest is
            // empty and the strip shows just its frame.
            if (now >= m_next_compass_recompute)
            {
                m_next_compass_recompute = now + std::chrono::milliseconds(1000);
                compute_nearest(px, py);
                // Overlay the LIVE actor layers (eggs/dungeons/lucky). compute_nearest just
                // rebuilt m_nearest from the map-open pool, which is stale for things that spawn/
                // stream while walking -- refresh_live_nearest keeps m_live_nearest fresh off its
                // own scan, so inject each here. A layer with no scanned entry yet is LEFT as the
                // pool value (avoids a brief vanish before its first round-robin scan).
                struct LiveMeta
                {
                    int layer;
                    const wchar_t* icon;
                    Engine::FLinearColor_ color;
                };
                static const LiveMeta kLiveMeta[] = {
                    {kEggLayer, nullptr, {1.0f, 0.82f, 0.15f, 1.0f}},
                    {kDungeonLayer, kDungeonIcon, kDungeonColor},
                    {kLuckyLayer, nullptr, kLuckyColor},
                };
                for (const auto& lm : kLiveMeta)
                {
                    auto it = m_live_nearest.find(lm.layer);
                    if (it == m_live_nearest.end())
                    {
                        continue;   // not scanned yet: keep whatever compute_nearest set
                    }
                    if (it->second.valid && is_layer_on(lm.layer))
                    {
                        m_nearest[lm.layer] =
                            Nearest{it->second.dist, 0.0, it->second.wx, it->second.wy, lm.icon, lm.color};
                    }
                    else
                    {
                        m_nearest.erase(lm.layer);
                    }
                }
            }
            // NO idle-skip (2026-07-20). It used to early-return here when the player had not
            // moved -- but that STRANDED markers collapsed: once a transient placed=0 (map
            // close / dots cleared / icons not ready at first place) hid them via the
            // collapse-unused loop below, standing still kept them hidden forever ("dark bar,
            // no icons", Kenny). The expensive part -- compute_nearest -- is already throttled
            // to 1 Hz above; the place loop below is a handful of SetOffsets, cheap enough to
            // run every tick so the markers always reflect the current nearest set.
            m_compass_last_px = px;
            m_compass_last_py = py;
            m_compass_last_yaw = yaw;
            m_compass_have_last = true;

            auto* image_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
            auto* txt_class =
                UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
            if (!image_class || !txt_class)
            {
                return;
            }
            const double half = g_compass_w / 2.0;
            const double arc = std::max(10.0, g_compass_arc);
            const double kpx = half / arc;   // px per degree across the shown arc
            const bool icons = m_icons_ready.load(std::memory_order_acquire);
            const double isz = std::clamp(g_compass_h * 0.5, 16.0, 28.0);
            // Camera forward bearing (clockwise from north), matching the target bearings
            // wb = atan2(dx=east, dy=north). The game's yaw increases CLOCKWISE (turn right
            // = +yaw), so 90+yaw gets the turn RESPONSE right (d(rel)/d(yaw) = 1). The
            // absolute reference is the flaky part: the +180 that used to live here was never
            // validated in-game (the compass never showed on a server until 2026-07-20), and
            // once it did, walking to a centred marker moved you AWAY -- i.e. +180 was one
            // half-turn too far. Playtest chain: 90+yaw+180 = "farther away" (180 off);
            // 90+yaw (offset 0, flip off) = "NE shows NW" (left/right MIRROR -> CompassFlipEast);
            // 90+yaw flip-on offset 0 = "north shows south" (clean 180) -> offset 180. So the
            // shipped defaults are CompassFlipEast=on + CompassYawOffset=180, both runtime-
            // tunable so any residual is a settings edit, not a rebuild (mouse-look can't be
            // scripted).
            const double cam_bearing = 90.0 + yaw + g_compass_yaw_offset;

            struct Place
            {
                double x;
                double dist;
                const wchar_t* icon;
                Engine::FLinearColor_ color;
                int layer;
            };
            std::vector<Place> places;
            int enabled_seen = 0, arc_culled = 0;   // dev: why the strip is empty
            std::wstring dbg;
            for (const auto& [layer_id, n] : m_nearest)
            {
                if (!is_layer_on(layer_id))
                {
                    continue;
                }
                ++enabled_seen;
                // Flip the east delta to mirror left/right (CompassFlipEast) -- see the global.
                const double dx = g_compass_flip_east ? (px - n.wx) : (n.wx - px);
                double wb = std::atan2(dx, n.wy - py) * 180.0 / 3.14159265358979323846;
                double rel = wb - cam_bearing;
                while (rel > 180.0)
                {
                    rel -= 360.0;
                }
                while (rel < -180.0)
                {
                    rel += 360.0;
                }
                if (dbg.size() < 400)
                {
                    // wb0 = the marker's TRUE world compass bearing (0=N,90=E,180=S,270=W),
                    // unflipped, camera-independent. With yaw + rel below, naming one marker's
                    // real direction pins the whole convention (calibration, not per-frame use).
                    double wb0 = std::atan2(n.wx - px, n.wy - py) * 180.0 / 3.14159265358979323846;
                    if (wb0 < 0) wb0 += 360.0;
                    wchar_t b[96];
                    swprintf(b, 96, L"%ls(wb=%.0f rel=%.0f d=%.0f) ", layer_key(layer_id), wb0, rel, n.dist_uu);
                    dbg += b;
                }
                if (rel < -arc || rel > arc)
                {
                    ++arc_culled;
                    continue;   // behind you / outside the shown arc
                }
                places.push_back({half + rel * kpx, n.dist_uu, n.icon, n.color, layer_id});
            }
            if (now >= m_next_compass_dbg)
            {
                m_next_compass_dbg = now + std::chrono::milliseconds(3000);
                Output::send<LogLevel::Default>(
                    STR("[Lodestone] compass: nearest={} enabled={} placed={} arc-culled={} arc=±{:.0f} "
                        "yaw={:.0f} | {}\n"),
                    m_nearest.size(), enabled_seen, places.size(), arc_culled, arc, yaw, dbg.c_str());
            }
            // Nearest last, so a closer marker draws over a farther one at the same spot.
            std::sort(places.begin(), places.end(),
                      [](const Place& a, const Place& b) { return a.dist > b.dist; });

            std::unordered_set<int> shown;
            for (const auto& p : places)
            {
                size_t idx = 0;
                auto ixit = m_compass_marker_ix.find(p.layer);
                if (ixit != m_compass_marker_ix.end())
                {
                    idx = ixit->second;
                }
                else
                {
                    CompassMarker m{};
                    {
                        FStaticConstructObjectParameters params{image_class, m_compass_canvas};
                        m.icon = UObjectGlobals::StaticConstructObject(params);
                        if (!m.icon)
                        {
                            continue;
                        }
                        Engine::ParamsAddChildToCanvas a{m.icon, nullptr};
                        if (!Engine::call(m_compass_canvas, L"AddChildToCanvas", a) || !a.ReturnValue)
                        {
                            continue;
                        }
                        m.icon_slot = a.ReturnValue;
                        Engine::ParamsSetAnchors an{0, 0.5, 0, 0.5};
                        Engine::call(m.icon_slot, L"SetAnchors", an);
                        Engine::ParamsSetAlignment al{{0.5, 0.5}};
                        Engine::call(m.icon_slot, L"SetAlignment", al);
                    }
                    {
                        FStaticConstructObjectParameters params{txt_class, m_compass_canvas};
                        m.label = UObjectGlobals::StaticConstructObject(params);
                        if (m.label)
                        {
                            Engine::ParamsAddChildToCanvas a{m.label, nullptr};
                            if (Engine::call(m_compass_canvas, L"AddChildToCanvas", a) && a.ReturnValue)
                            {
                                m.label_slot = a.ReturnValue;
                                Engine::ParamsSetAnchors an{0, 0.5, 0, 0.5};
                                Engine::call(m.label_slot, L"SetAnchors", an);
                                Engine::ParamsSetAlignment al{{0.5, 0.5}};
                                Engine::call(m.label_slot, L"SetAlignment", al);
                                // TextBlock.SetColorAndOpacity takes an FSlateColor, not a
                                // bare FLinearColor -- the missing ColorUseRule byte read
                                // stack garbage and flipped the text white<->black. Pass a
                                // full FSlateColor with rule 0 (UseColor_Specified).
                                Engine::ParamsSetTextColor tc{{1.0f, 1.0f, 1.0f, 0.95f}, 0};
                                Engine::call(m.label, L"SetColorAndOpacity", tc);
                                // Writing Font.Size directly did NOT resize an already-
                                // parented+measured label -- the text stayed ~24px and
                                // buried the icons (Kenny, twice). Shrink it reliably with a
                                // render-transform scale (same primitive the dots use) to
                                // roughly match vanilla's tiny compass distance.
                                Engine::ParamsSetRenderScale rsz{{0.45, 0.45}};
                                Engine::call(m.label, L"SetRenderScale", rsz);
                            }
                        }
                    }
                    idx = m_compass_markers.size();
                    m_compass_markers.push_back(m);
                    m_compass_marker_ix.emplace(p.layer, idx);
                }
                CompassMarker& m = m_compass_markers[idx];
                // Art: item icon (white tint) / a glyph tinted with the layer colour /
                // a plain coloured dot until icons load. Only re-brush on a change.
                const bool glyph = p.icon && Style::icon_is_glyph(p.icon);
                if (m.cur_icon != p.icon || m.cur_glyph != glyph)
                {
                    UObject* tex = (icons && p.icon) ? layer_texture(p.icon) : nullptr;
                    if (tex)
                    {
                        Engine::ParamsSetBrushFromTexture b{tex, false};
                        Engine::call(m.icon, L"SetBrushFromTexture", b);
                        Style::make_image(m.icon, isz);
                        Engine::ParamsSetColorAndOpacity c{
                            glyph ? p.color : Engine::FLinearColor_{1.0f, 1.0f, 1.0f, 1.0f}};
                        Engine::call(m.icon, L"SetColorAndOpacity", c);
                    }
                    else
                    {
                        Style::make_round(m.icon, isz / 2.0);
                        Engine::ParamsSetColorAndOpacity c{p.color};
                        Engine::call(m.icon, L"SetColorAndOpacity", c);
                    }
                    m.cur_icon = tex ? p.icon : nullptr;
                    m.cur_glyph = glyph;
                }
                Engine::ParamsSetOffsets io{static_cast<float>(p.x), -7.0f, static_cast<float>(isz),
                                            static_cast<float>(isz)};
                Engine::call(m.icon_slot, L"SetOffsets", io);
                Engine::ParamsSetVisibility iv{Engine::Vis_HitTestInvisible};
                Engine::call(m.icon, L"SetVisibility", iv);
                if (m.label && m.label_slot)
                {
                    const double meters = p.dist / 100.0;
                    wchar_t buf[24];
                    if (meters >= 1000.0)
                    {
                        std::swprintf(buf, std::size(buf), L"%.1fkm", meters / 1000.0);
                    }
                    else
                    {
                        std::swprintf(buf, std::size(buf), L"%.0fm", meters);
                    }
                    if (m.cur_text != buf)
                    {
                        Engine::ParamsSetText st{FText(buf)};
                        Engine::call(m.label, L"SetText", st);
                        m.cur_text = buf;
                    }
                    Engine::ParamsSetOffsets lo{static_cast<float>(p.x), 11.0f, 38.0f, 12.0f};
                    Engine::call(m.label_slot, L"SetOffsets", lo);
                    Engine::ParamsSetVisibility lv{Engine::Vis_HitTestInvisible};
                    Engine::call(m.label, L"SetVisibility", lv);
                }
                // Nearest draws on top: z-order by distance, since markers are no longer
                // created in distance order (each belongs to a fixed layer now).
                const int z = 1000 - static_cast<int>(std::min(999.0, p.dist / 100.0));
                Engine::ParamsSetZOrder zo{z};
                Engine::call(m.icon_slot, L"SetZOrder", zo);
                if (m.label_slot)
                {
                    Engine::call(m.label_slot, L"SetZOrder", zo);
                }
                shown.insert(p.layer);
            }
            // Collapse each layer whose marker is NOT shown this frame. A layer only ever
            // hides its OWN marker -- never reassigned to another layer -- so a marker can't
            // strand showing a stale effigy ("2 where 1") or flip icons as you turn.
            for (const auto& [layer, ix] : m_compass_marker_ix)
            {
                if (shown.count(layer))
                {
                    continue;
                }
                Engine::ParamsSetVisibility v{Engine::Vis_Collapsed};
                if (m_compass_markers[ix].icon)
                {
                    Engine::call(m_compass_markers[ix].icon, L"SetVisibility", v);
                }
                if (m_compass_markers[ix].label)
                {
                    Engine::call(m_compass_markers[ix].label, L"SetVisibility", v);
                }
            }
        }

        auto tick() -> void
        {
            // Before find_map: the title screen has no map, so anything below the
            // early-out never runs there -- which is exactly where autoload lives.
            try_autoload();
            tick_compass();
            tick_map_guarded();
        }
        // The map/panel/dot work below touches UMG on the on_update thread. On a server
        // the game streams the map body in and out from under us; touching a freed widget
        // mid-teardown is an ACCESS VIOLATION (not a C++ throw) and crashed the game when
        // Kenny toggled a layer with the map open (pool bounded at 1401, so not the old
        // widget-count spike -- a teardown race). Guard it exactly like the per-frame
        // minimap driver (minimap_frame): an outer C++ catch for a ProcessEvent-not-
        // available throw during a transition, and an inner SEH that swallows the AV and
        // skips the frame. Split across functions -- MSVC forbids __try in a function that
        // also has C++ objects needing unwind (which tick_map is full of).
        auto tick_map_guarded() -> void
        {
            try
            {
                tick_map_seh();
            }
            catch (...)
            {
                Output::send<LogLevel::Warning>(STR("[Lodestone] tick_map: C++ throw caught (frame skipped)\n"));
            }
            // Report a caught AV from the C++ layer (the __except scope stays object-free).
            if (m_seh_new)
            {
                m_seh_new = false;
                Output::send<LogLevel::Warning>(STR("[Lodestone] tick_map: SEH caught AV #{} (frame skipped)\n"),
                                                m_seh_hits);
            }
        }
        auto tick_map_seh() -> void
        {
            __try
            {
                tick_map();
            }
            __except (minimap_av_filter(GetExceptionCode()))
            {
                ++m_seh_hits;
                m_seh_new = true;
            }
        }
        auto tick_map() -> void
        {
            // The per-frame EngineTick hook normally drives input + minimap + compass at
            // 30 Hz. It is the compass's ONLY driver once m_minimap_fast is set -- but on a
            // server that hook fires during the startup tick then goes silent when the
            // world loads, freezing the compass empty ("stopped showing things again",
            // Kenny). Detect that stall via the hook's heartbeat and re-drive from here at
            // the on_update rate (~3 Hz, the pre-per-frame behaviour that worked). The
            // staleness window keeps the two drivers mutually exclusive: the hook stamps
            // m_last_frame_hook every frame, so a live hook keeps this branch out; a dead
            // one lets it in within ~1 s. Never m_minimap_fast (hook never installed) also
            // routes here.
            const auto tnow = std::chrono::steady_clock::now();
            const bool hook_stalled =
                !m_minimap_fast ||
                (tnow - m_last_frame_hook) > std::chrono::milliseconds(1000);
            if (hook_stalled)
            {
                handle_minimap_input();
                refresh_live_nearest();   // live actor-layer scan + lucky alert (fallback driver)
                // Compass before minimap here too: tick_map_seh's __except would otherwise
                // skip the compass whenever the (fragile) minimap tick AV'd mid-transition.
                build_compass();
                tick_compass_strip();
                build_minimap();
                tick_minimap();
            }
            UObject* root = nullptr;
            UObject* map_body_canvas = nullptr;
            UObject* mask = nullptr;
            if (!find_map(root, map_body_canvas, mask))
            {
                // map closed: collapse our overlay once (widgets stay pooled)
                if (m_layer_canvas && !m_collapsed)
                {
                    Engine::ParamsSetVisibility vis{Engine::Vis_Collapsed};
                    Engine::call(m_layer_canvas, L"SetVisibility", vis);
                    m_collapsed = true;
                }
                m_menu_toggle_req.store(false);   // map closed: an F5 press is a no-op, not queued
                return;
            }

            // ⚠ UE4SS object identity is only stable via full names (SPEC 2.3)
            const std::wstring full_name = mask->GetFullName();
            if (full_name != m_canvas_full_name)
            {
                // new map body instance: old widgets died with the previous tree
                m_canvas_full_name = full_name;
                m_layer_canvas = nullptr;
                m_layer_slot = nullptr;
                m_dots.clear();
                m_reveal_queue.clear();   // indices point into the pool that just died
                m_guid_dots.clear();
                m_pending_layers.clear();   // a full re-place (m_placed=false below) supersedes them
                m_apply_pending = false;
                m_emit_cursor = 0;
                m_applied_zoom = 1.0;
                // Only the paint cursor resets: the dots died with the tree and
                // must be repainted, but m_tex_index holds textures, not widgets.
                // Those are pinned for the life of the process and survive the map
                // closing -- and the loader has already removed itself, so clearing
                // the index here would lose the icons permanently.
                m_icon_scan = 0;
                m_recheck_cal = true;   // re-evaluate on the new body, but KEEP the best so far
                m_placed = false;
                m_collapsed = true;
                // NOT the panel. It is parented to WBP_Map_Base, which OUTLIVES the
                // map body -- so dropping it here does not free it, it orphans it: the
                // old canvas keeps rendering on the still-alive base screen while we
                // build a second one that is the only one being polled. That is the
                // doubled legend. build_panel owns the panel's lifetime instead, keyed
                // on its own root's identity.
            }

            if (!m_calibration || m_recheck_cal)
            {
                m_recheck_cal = false;
                std::vector<Project::Vec2> boss_pins, statue_pins;
                read_pins(mask, L"WBP_Map_IconTower_C", boss_pins);
                read_pins(mask, L"WBP_Map_IconFTTower_C", statue_pins);
                if (boss_pins.size() < 4 || statue_pins.size() < 100)
                {
                    if (!m_calibration)
                    {
                        m_recheck_cal = true;   // pins still populating, nothing cached: retry
                        return;
                    }
                    // else: keep the cached calibration and place with it this open
                }
                else
                {
                    std::vector<Project::Vec2> boss_world, statue_world;
                    for (const auto& p : Data::kBossTowers)
                    {
                        boss_world.push_back({static_cast<double>(p.x), static_cast<double>(p.y)});
                    }
                    for (const auto& p : Data::kStatues)
                    {
                        statue_world.push_back({static_cast<double>(p.x), static_cast<double>(p.y)});
                    }
                    const auto cand = Project::calibrate(boss_world, boss_pins, statue_world, statue_pins);
                    // Keep the BEST calibration across body swaps and REJECT a worse open.
                    // The transform is a fixed map property, but a server drops a boss/FT pin
                    // some opens -> farthest-pair picks the wrong seed correspondence and the
                    // whole fit blows up (seed 1106px, 27 outliers). Re-rolling every open was
                    // breaking a previously-good session; caching the best self-heals instead.
                    if (cand &&
                        (!m_calibration || cand->refine_residual_px < m_calibration->refine_residual_px))
                    {
                        m_calibration = cand;
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] calibrated: seed {:.1f}px refine {:.2f}px ({} anchors)\n"),
                            m_calibration->seed_residual_px, m_calibration->refine_residual_px,
                            m_calibration->matched_statues);
                        // Good fit: refine <1px, max <~5px. Big max at a clustered worst_world
                        // = regional drift; many >50px / statue_pins far from world = the pin
                        // set shifted under our fixed anchors. worst_world is in game uu.
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] calibration detail: max {:.1f}px at world({:.0f},{:.0f}); {} of "
                                "{} matched >50px; statues world={} pins={} matched={}; boss pins={}\n"),
                            m_calibration->max_residual_px, m_calibration->worst_world.x,
                            m_calibration->worst_world.y, m_calibration->anchors_over_50px,
                            m_calibration->matched_statues, m_calibration->statue_world_n,
                            m_calibration->statue_pins_n, m_calibration->matched_statues,
                            m_calibration->boss_pins_n);
                    }
                    else if (cand)
                    {
                        Output::send<LogLevel::Default>(
                            STR("[Lodestone] calibration kept cached (refine {:.2f}px) over worse open "
                                "(seed {:.1f}px refine {:.2f}px, {} anchors >50px)\n"),
                            m_calibration->refine_residual_px, cand->seed_residual_px,
                            cand->refine_residual_px, cand->anchors_over_50px);
                    }
                    else if (!m_calibration)
                    {
                        log_once(L"calibration failed");
                        return;
                    }
                }
                if (!m_calibration)
                {
                    return;
                }
            }

            if (!ensure_layer_canvas(map_body_canvas, mask))
            {
                return;
            }
            sync_layer_geometry(mask);   // follow zoom / layout changes
            if (m_placed && !m_collapsed)
            {
                sync_dot_scale(mask);        // keep dots readable across zoom levels
                paint_icons_batch(400);      // stream icon textures in, non-blocking
            }
            if (!m_placed)
            {
                place_dots();
                apply_layer_visibility();   // honor toggles from the start
                log_player_context();
            }
            else if (m_collapsed)
            {
                // Collapse the on-layers before re-showing the canvas, then let
                // refresh_collected re-queue them: the dots stayed individually visible
                // across the close, so showing the parent would otherwise reveal a dense
                // layer all at once and spike Slate (the RedBerry crash, on reopen).
                collapse_shown_dots();
                Engine::ParamsSetVisibility vis{Engine::Vis_SelfHitTestInvisible};
                Engine::call(m_layer_canvas, L"SetVisibility", vis);
                m_collapsed = false;
                // Re-scan the live layers so a picked-up egg / just-unlocked tower drops out
                // (the static layers below m_live_begin are untouched). Must run BEFORE
                // refresh_collected: its apply_layer_visibility is what queues the freshly
                // re-emitted live dots for the budgeted reveal below.
                refresh_live_layers();
                refresh_collected();   // re-queues the on-layers -> drain_reveals stages
                // Every open, not just the first: m_placed only resets when the map
                // widget dies, so this is the only path a reopen takes. Logging it per
                // open is what makes it usable as a probe -- walk somewhere, reopen,
                // read the position.
                log_player_context();
            }

            // Reveal a bounded slice of any pending Collapsed->Visible flips this frame,
            // so a dense layer coming on (toggle or map-open) can't spike Slate.
            drain_reveals();

            // F5 (game-thread consume): toggle the config popup. We are past find_map, so
            // the map is open. open/cancel set m_panel_relayout -> layout_panel renders below.
            if (m_menu_toggle_req.exchange(false))
            {
                m_menu_open ? cancel_menu() : open_menu();
            }
            // interface panel: build once, then poll toggles each tick
            build_panel(root);
            poll_panel();
            drain_pending_apply();   // apply debounced toggles once the click-burst settles
            if (m_panel_relayout)
            {
                m_panel_relayout = false;
                layout_panel();
            }
        }
    };
} // namespace CairnMap

#define MOD_EXPORT __declspec(dllexport)
extern "C"
{
    MOD_EXPORT RC::CppUserModBase* start_mod()
    {
        return new CairnMap::Mod();
    }
    MOD_EXPORT void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
