// Writes CouriersLedger.esp: a light plugin (ESPFE) with the quest CL_TrackQuest.
// The quest has one reference alias (Target, id 0) and one objective (index 10) that points at it.
// The DLL fills the alias with the person the player picks and shows the objective, which gives the
// person a quest marker on the map and the compass. The quest never starts by itself.

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Skyrim;

var folder = args.Length > 0 ? args[0] : ".";
Directory.CreateDirectory(folder);
var path = Path.Combine(folder, "CouriersLedger.esp");

var mod = new SkyrimMod(ModKey.FromFileName("CouriersLedger.esp"), SkyrimRelease.SkyrimSE);
mod.ModHeader.Flags |= SkyrimModHeader.HeaderFlag.Small;
mod.ModHeader.Author = "dqann and Claude";
mod.ModHeader.Description = "The Courier's Ledger - the tracking quest";
mod.ModHeader.Stats.NextFormID = 0x800;

var quest = mod.Quests.AddNew("CL_TrackQuest");
quest.Name = "The Courier's Ledger";
quest.Priority = 0;
quest.QuestFormVersion = 44;
quest.Type = Quest.TypeEnum.Misc;

quest.Aliases.Add(new QuestAlias
{
    ID = 0,
    Type = QuestAlias.TypeEnum.Reference,
    Name = "Target",
    Flags = QuestAlias.Flag.Optional
          | QuestAlias.Flag.AllowReserved
          | QuestAlias.Flag.AllowReuseInQuest
          | QuestAlias.Flag.AllowDead
          | QuestAlias.Flag.AllowDisabled,
});
quest.NextAliasID = 1;

var objective = new QuestObjective
{
    Index = 10,
    DisplayText = "Find <Alias=Target>",
};
objective.Targets.Add(new QuestObjectiveTarget
{
    AliasID = 0,
    Flags = Quest.TargetFlag.CompassMarkerIgnoresLocks,
});
quest.Objectives.Add(objective);
quest.Stages.Add(new QuestStage { Index = 0 });

mod.BeginWrite
    .ToPath(path)
    .WithNoLoadOrder()
    .Write();

var bytes = File.ReadAllBytes(path);
Console.WriteLine($"Wrote {path}: {bytes.Length} bytes, quest {quest.FormKey}, small flag {(mod.ModHeader.Flags & SkyrimModHeader.HeaderFlag.Small) != 0}");
