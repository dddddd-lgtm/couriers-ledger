// Writes CouriersLedger.esp: a light plugin (ESPFE) with the quest CL_TrackQuest.
// The quest has 20 reference aliases (Target, id 0, and Target2 to Target20, ids 1 to 19) and two objectives.
// Objective 10 points at alias 0: the DLL fills it with the one person the player picks and shows the
// objective, which gives the person a quest marker on the map and the compass. Objective 20 points at all
// 20 aliases: "Track all" fills them with the nearest people from a search and shows that objective
// instead. Aliases left empty get no marker. The quest never starts by itself.

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

const int aliasCount = 20;
for (var id = 0; id < aliasCount; ++id)
{
    quest.Aliases.Add(new QuestAlias
    {
        ID = (uint)id,
        Type = QuestAlias.TypeEnum.Reference,
        Name = id == 0 ? "Target" : $"Target{id + 1}",
        Flags = QuestAlias.Flag.Optional
              | QuestAlias.Flag.AllowReserved
              | QuestAlias.Flag.AllowReuseInQuest
              | QuestAlias.Flag.AllowDead
              | QuestAlias.Flag.AllowDisabled,
    });
}
quest.NextAliasID = aliasCount;

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

var all = new QuestObjective
{
    Index = 20,
    DisplayText = "Find the people you are tracking",
};
for (var id = 0; id < aliasCount; ++id)
{
    all.Targets.Add(new QuestObjectiveTarget
    {
        AliasID = id,
        Flags = Quest.TargetFlag.CompassMarkerIgnoresLocks,
    });
}
quest.Objectives.Add(all);
quest.Stages.Add(new QuestStage { Index = 0 });

mod.BeginWrite
    .ToPath(path)
    .WithNoLoadOrder()
    .Write();

var bytes = File.ReadAllBytes(path);
Console.WriteLine($"Wrote {path}: {bytes.Length} bytes, quest {quest.FormKey}, {quest.Aliases.Count} aliases, {quest.Objectives.Count} objectives, small flag {(mod.ModHeader.Flags & SkyrimModHeader.HeaderFlag.Small) != 0}");
