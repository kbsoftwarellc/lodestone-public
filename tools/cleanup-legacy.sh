#!/usr/bin/env bash
# Removes the legacy Map Collectables Helper stack once CairnMap replaces it.
# Run ONLY after CairnMap (C++ DLL + UI pak) is installed and verified in game.
set -euo pipefail
PAL="${PAL:-$HOME/.local/share/Steam/steamapps/common/Palworld}"
UE4SS="$PAL/Mods/NativeMods/UE4SS/Mods"
LM="$PAL/Pal/Content/Paks/LogicMods"

echo "== legacy pak + config + image overrides =="
rm -fv "$LM/MapCollectablesMod.pak" "$LM/MapCollectablesMod.modconfig.json"
rm -rfv "$LM/MapCollectablesMod.ImageOverrides"

echo "== runtime Lua fix =="
rm -rfv "$UE4SS/MapCollectablesFix"
sed -i 's/^MapCollectablesFix : 1.*$//' "$UE4SS/mods.txt"

echo "== reminder (manual) =="
echo " - Se désabonner du Workshop item 3704720562 (Map Collectables Helper)"
echo "   sinon le jeu re-télécharge le pak dans workshop/content/ (inoffensif mais inutile)"
echo " - Retirer 'MapCollectables' de ActiveModList dans $PAL/Mods/PalModSettings.ini"
echo "Nettoyage terminé."
