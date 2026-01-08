/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Chat.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Transmogrification.h"
#include "DatabaseEnv.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <cctype>
#include <sstream>
#include <optional>
#include <string>
#include <stdexcept>
#include <algorithm>

using namespace Acore::ChatCommands;

class transmog_commandscript : public CommandScript
{
public:
    transmog_commandscript() : CommandScript("transmog_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable addCollectionTable =
        {
            { "set", HandleAddTransmogItemSet,    SEC_MODERATOR, Console::Yes },
            { "",    HandleAddTransmogItem,       SEC_MODERATOR, Console::Yes },
        };

        static ChatCommandTable transmogTable =
        {
            { "add",       addCollectionTable                                        },
            { "",          HandleDisableTransMogVisual,   SEC_PLAYER,    Console::No },
            { "sync",      HandleSyncTransMogCommand,     SEC_PLAYER,    Console::No },
            { "apply",     HandleApplyTransmogCommand,     SEC_PLAYER,    Console::No },
            { "hide",      HandleHideTransmogCommand,      SEC_PLAYER,    Console::No },
            { "portable",  HandleTransmogPortableCommand, SEC_PLAYER,    Console::No },
            { "interface", HandleInterfaceOption,         SEC_PLAYER,    Console::No },
            { "save",      HandleSaveSetCommand,          SEC_PLAYER,    Console::No },
            { "load",      HandleLoadSetCommand,          SEC_PLAYER,    Console::No },
            { "list",      HandleListSetsCommand,         SEC_PLAYER,    Console::No },
            { "delete",    HandleDeleteSetCommand,        SEC_PLAYER,    Console::No },
            { "help",      HandleHelpCommand,             SEC_PLAYER,    Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "transmog", transmogTable },
        };

        return commandTable;
    }

    static bool HandleSyncTransMogCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        handler->SendSysMessage(LANG_CMD_TRANSMOG_BEGIN_SYNC);
        sTransmogrification->SendFullSync(player);
        handler->SendSysMessage(LANG_CMD_TRANSMOG_COMPLETE_SYNC);
        return true;
    }

    static bool ValidateSetName(std::string& name, ChatHandler* handler)
    {
        auto trim = [](std::string& s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };

        trim(name);
        if (name.empty())
            return false;

        for (unsigned char c : name)
        {
            if (!(std::isalnum(c) || c == ' '))
                return false;
        }

        return true;
    }

    static bool HandleSaveSetCommand(ChatHandler* handler, Tail nameTail)
    {
        if (!sTransmogrification->GetEnableSets())
        {
            handler->SendSysMessage("Transmog sets are disabled.");
            return true;
        }

        std::string name(nameTail.data(), nameTail.size());
        if (!ValidateSetName(name, handler))
        {
            handler->SendSysMessage("INVALID - Use letters, numbers, and spaces only");
            return true;
        }

        Player* player = handler->GetPlayer();

        // Exact name match to decide whether we overwrite an existing preset.
        std::optional<uint8> overwritePreset;
        for (auto const& it : sTransmogrification->presetByName[player->GetGUID()])
        {
            if (it.second == name)
            {
                overwritePreset = it.first;
                break;
            }
        }

        if (!overwritePreset && sTransmogrification->presetByName[player->GetGUID()].size() >= sTransmogrification->GetMaxSets())
        {
            handler->SendSysMessage("You have reached the maximum number of saved sets.");
            return true;
        }

        int32 cost = 0;
        std::map<uint8, uint32> items;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (!sTransmogrification->GetSlotName(slot, player->GetSession()))
                continue;
            if (Item* newItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                uint32 entry = sTransmogrification->GetFakeEntry(newItem->GetGUID());
                if (!entry)
                    continue;
                if (entry != HIDDEN_ITEM_ID)
                {
                    const ItemTemplate* temp = sObjectMgr->GetItemTemplate(entry);
                    if (!temp)
                        continue;
                    if (!sTransmogrification->SuitableForTransmogrification(player, temp))
                        continue;
                    cost += sTransmogrification->GetSpecialPrice(temp);
                }
                items[slot] = entry;
            }
        }

        if (items.empty())
        {
            handler->SendSysMessage("No transmogrified items to save.");
            return true;
        }

        cost *= sTransmogrification->GetSetCostModifier();
        cost += sTransmogrification->GetSetCopperCost();
        if (!player->HasEnoughMoney(cost))
        {
            ChatHandler(player->GetSession()).SendNotification(LANG_ERR_TRANSMOG_NOT_ENOUGH_MONEY);
            return true;
        }

        uint8 presetIdToUse = UINT8_MAX;
        if (overwritePreset)
        {
            presetIdToUse = *overwritePreset;
        }
        else
        {
            for (uint8 presetID = 0; presetID < sTransmogrification->GetMaxSets(); ++presetID)
            {
                if (sTransmogrification->presetByName[player->GetGUID()].find(presetID) == sTransmogrification->presetByName[player->GetGUID()].end())
                {
                    presetIdToUse = presetID;
                    break;
                }
            }
            if (presetIdToUse == UINT8_MAX)
            {
                handler->SendSysMessage("No free preset slots available.");
                return true;
            }
        }

        std::ostringstream ss;
        // Clear old cached entries when overwriting.
        sTransmogrification->presetById[player->GetGUID()][presetIdToUse].clear();
        for (auto const& it : items)
        {
            ss << uint32(it.first) << ' ' << it.second << ' ';
            sTransmogrification->presetById[player->GetGUID()][presetIdToUse][it.first] = it.second;
        }
        sTransmogrification->presetByName[player->GetGUID()][presetIdToUse] = name;
        CharacterDatabase.Execute("REPLACE INTO `custom_transmogrification_sets` (`Owner`, `PresetID`, `SetName`, `SetData`) VALUES ({}, {}, \"{}\", \"{}\")", player->GetGUID().GetCounter(), uint32(presetIdToUse), name, ss.str());
        if (cost)
            player->ModifyMoney(-cost);

        {
            std::string line = Acore::StringFormat("Saved set {}: {}", uint32(presetIdToUse) + 1, name);
            handler->SendSysMessage(line.c_str());
        }
        return true;
    }

    static bool HandleDeleteSetCommand(ChatHandler* handler, Tail nameTail)
    {
        if (!sTransmogrification->GetEnableSets())
        {
            handler->SendSysMessage("Transmog sets are disabled.");
            return true;
        }

        std::string name(nameTail.data(), nameTail.size());
        if (!ValidateSetName(name, handler))
        {
            handler->SendSysMessage("INVALID - Use letters, numbers, and spaces only");
            return true;
        }

        Player* player = handler->GetPlayer();
        auto& presets = sTransmogrification->presetByName[player->GetGUID()];
        std::optional<uint8> presetId;
        for (auto const& it : presets)
        {
            if (it.second == name)
            {
                presetId = it.first;
                break;
            }
        }

        if (!presetId)
        {
            handler->SendSysMessage("Set not found.");
            return true;
        }

        sTransmogrification->presetByName[player->GetGUID()].erase(*presetId);
        sTransmogrification->presetById[player->GetGUID()].erase(*presetId);
        CharacterDatabase.Execute("DELETE FROM `custom_transmogrification_sets` WHERE Owner = {} AND PresetID = {}", player->GetGUID().GetCounter(), uint32(*presetId));

        {
            std::string line = Acore::StringFormat("Deleted set {}: {}", uint32(*presetId) + 1, name);
            handler->SendSysMessage(line.c_str());
        }
        return true;
    }

    static std::optional<uint8> ResolvePresetId(Player* player, std::string const& input)
    {
        // try numeric 1-based
        bool numeric = !input.empty() && std::all_of(input.begin(), input.end(), ::isdigit);
        if (numeric)
        {
            uint32 idx = 0;
            try
            {
                idx = static_cast<uint32>(std::stoul(input));
            }
            catch (...)
            {
                idx = 0;
            }
            if (idx == 0)
                return std::nullopt;
            uint8 presetId = uint8(idx - 1);
            if (sTransmogrification->presetByName[player->GetGUID()].count(presetId))
                return presetId;
        }

        // case-insensitive name match
        std::string lower = input;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (auto const& it : sTransmogrification->presetByName[player->GetGUID()])
        {
            std::string candidate = it.second;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), ::tolower);
            if (candidate == lower)
                return it.first;
        }

        return std::nullopt;
    }

    static bool HandleLoadSetCommand(ChatHandler* handler, Tail nameTail)
    {
        if (!sTransmogrification->GetEnableSets())
        {
            handler->SendSysMessage("Transmog sets are disabled.");
            return true;
        }

        std::string input(nameTail.data(), nameTail.size());
        if (input.empty())
        {
            handler->SendSysMessage("Usage: .transmog load <name|number>");
            return false;
        }

        Player* player = handler->GetPlayer();
        auto presetOpt = ResolvePresetId(player, input);
        if (!presetOpt)
        {
            handler->SendSysMessage("Set not found.");
            return true;
        }
        uint8 presetId = *presetOpt;

        for (auto const& it : sTransmogrification->presetById[player->GetGUID()][presetId])
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, it.first))
                sTransmogrification->PresetTransmog(player, item, it.second, it.first);
        }

        {
            std::string line = Acore::StringFormat("Loaded set {}: {}", uint32(presetId) + 1, sTransmogrification->presetByName[player->GetGUID()][presetId]);
            handler->SendSysMessage(line.c_str());
        }
        return true;
    }

    static bool HandleListSetsCommand(ChatHandler* handler)
    {
        if (!sTransmogrification->GetEnableSets())
        {
            handler->SendSysMessage("Transmog sets are disabled.");
            return true;
        }

        Player* player = handler->GetPlayer();
        if (sTransmogrification->presetByName[player->GetGUID()].empty())
        {
            handler->SendSysMessage("No saved sets.");
            return true;
        }

        handler->SendSysMessage("Saved transmog sets:");
        for (auto const& it : sTransmogrification->presetByName[player->GetGUID()])
        {
            std::string line = Acore::StringFormat("  {}: {}", uint32(it.first) + 1, it.second);
            handler->SendSysMessage(line.c_str());
        }
        return true;
    }

    static bool HandleHelpCommand(ChatHandler* handler)
    {
        handler->SendSysMessage("Transmog commands:");
        handler->SendSysMessage("  .transmog save <name>  - Save current transmogs as a set (names allow letters/numbers/spaces)");
        handler->SendSysMessage("  .transmog load <name|number> - Load a saved set");
        handler->SendSysMessage("  .transmog delete <name> - Delete a saved set (exact name, case sensitive)");
        handler->SendSysMessage("  .transmog list - List your saved sets");
        handler->SendSysMessage("  .transmog sync - Sync appearances");
        return true;
    }

    static uint8 ResolveSlot(std::string slotName)
    {
        std::transform(slotName.begin(), slotName.end(), slotName.begin(), ::tolower);

        static const std::unordered_map<std::string, uint8> slotMap = {
            { "head",        EQUIPMENT_SLOT_HEAD },
            { "neck",        EQUIPMENT_SLOT_NECK },
            { "shoulder",    EQUIPMENT_SLOT_SHOULDERS },
            { "shirt",       EQUIPMENT_SLOT_BODY },
            { "chest",       EQUIPMENT_SLOT_CHEST },
            { "waist",       EQUIPMENT_SLOT_WAIST },
            { "legs",        EQUIPMENT_SLOT_LEGS },
            { "feet",        EQUIPMENT_SLOT_FEET },
            { "wrist",       EQUIPMENT_SLOT_WRISTS },
            { "hands",       EQUIPMENT_SLOT_HANDS },
            { "finger1",     EQUIPMENT_SLOT_FINGER1 },
            { "finger2",     EQUIPMENT_SLOT_FINGER2 },
            { "trinket1",    EQUIPMENT_SLOT_TRINKET1 },
            { "trinket2",    EQUIPMENT_SLOT_TRINKET2 },
            { "back",        EQUIPMENT_SLOT_BACK },
            { "main",        EQUIPMENT_SLOT_MAINHAND },
            { "mh",          EQUIPMENT_SLOT_MAINHAND },
            { "mainhand",    EQUIPMENT_SLOT_MAINHAND },
            { "off",         EQUIPMENT_SLOT_OFFHAND },
            { "oh",          EQUIPMENT_SLOT_OFFHAND },
            { "offhand",     EQUIPMENT_SLOT_OFFHAND },
            { "ranged",      EQUIPMENT_SLOT_RANGED },
            { "range",       EQUIPMENT_SLOT_RANGED },
            { "rng",         EQUIPMENT_SLOT_RANGED },
            { "tabard",      EQUIPMENT_SLOT_TABARD }
        };

        uint8 slot = EQUIPMENT_SLOT_END;
        if (auto it = slotMap.find(slotName); it != slotMap.end())
            slot = it->second;

        return slot;
    }

    static bool HandleApplyTransmogCommand(ChatHandler* handler, std::string slotName, ItemTemplate const* itemTemplate)
    {
        Player* player = handler->GetPlayer();
        if (!player || !itemTemplate)
            return false;

        if (!sTransmogrification->GetUseCollectionSystem())
        {
            handler->SendSysMessage("Transmog apply is only available when the collection system is enabled.");
            return true;
        }

        uint8 slot = ResolveSlot(slotName);
        if (slot >= EQUIPMENT_SLOT_END)
        {
            handler->SendSysMessage("Invalid slot.");
            return false;
        }

        uint32 accountId = player->GetSession()->GetAccountId();
        auto cacheIt = sTransmogrification->collectionCache.find(accountId);
        if (cacheIt == sTransmogrification->collectionCache.end() ||
            std::find(cacheIt->second.begin(), cacheIt->second.end(), itemTemplate->ItemId) == cacheIt->second.end())
        {
            handler->SendSysMessage("That appearance is not in your collection.");
            return true;
        }

        TransmogAcoreStrings res = sTransmogrification->Transmogrify(player, itemTemplate->ItemId, slot);
        if (res == LANG_ERR_TRANSMOG_OK)
            ChatHandler(player->GetSession()).SendNotification(LANG_ERR_TRANSMOG_OK);
        else
            ChatHandler(player->GetSession()).SendNotification(res);
        return true;
    }

    static bool HandleHideTransmogCommand(ChatHandler* handler, std::string slotName)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        uint8 slot = ResolveSlot(slotName);
        if (slot >= EQUIPMENT_SLOT_END)
        {
            handler->SendSysMessage("Invalid slot.");
            return false;
        }

        if (!sTransmogrification->GetAllowHiddenTransmog())
        {
            handler->SendSysMessage("Hidden transmogs are disabled.");
            return true;
        }

        TransmogAcoreStrings res = sTransmogrification->Transmogrify(player, UINT_MAX, slot);
        if (res == LANG_ERR_TRANSMOG_OK)
            ChatHandler(player->GetSession()).SendNotification(LANG_ERR_TRANSMOG_OK);
        else
            ChatHandler(player->GetSession()).SendNotification(res);
        return true;
    }

    static bool HandleDisableTransMogVisual(ChatHandler* handler, bool hide)
    {
        Player* player = handler->GetPlayer();

        if (hide)
        {
            player->UpdatePlayerSetting("mod-transmog", SETTING_HIDE_TRANSMOG, 0);
            handler->SendSysMessage(LANG_CMD_TRANSMOG_SHOW);
        }
        else
        {
            player->UpdatePlayerSetting("mod-transmog", SETTING_HIDE_TRANSMOG, 1);
            handler->SendSysMessage(LANG_CMD_TRANSMOG_HIDE);
        }

        player->UpdateObjectVisibility();
        return true;
    }

    static bool HandleAddTransmogItem(ChatHandler* handler, Optional<PlayerIdentifier> player, ItemTemplate const* itemTemplate)
    {
        if (!sTransmogrification->GetUseCollectionSystem())
            return true;

        if (!sObjectMgr->GetItemTemplate(itemTemplate->ItemId))
        {
            handler->PSendSysMessage(LANG_COMMAND_ITEMIDINVALID, itemTemplate->ItemId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!player)
            return false;

        Player* target = player->GetConnectedPlayer();
        bool isNotConsole = handler->GetSession();
        bool suitableForTransmog;

        if (target)
            suitableForTransmog = sTransmogrification->SuitableForTransmogrification(target, itemTemplate);
        else
            suitableForTransmog = sTransmogrification->SuitableForTransmogrification(player->GetGUID(), itemTemplate);

        if (!sTransmogrification->GetTrackUnusableItems() && !suitableForTransmog)
        {
            handler->SendSysMessage(LANG_CMD_TRANSMOG_ADD_UNSUITABLE);
            handler->SetSentErrorMessage(true);
            return true;
        }

        if (itemTemplate->Class != ITEM_CLASS_ARMOR && itemTemplate->Class != ITEM_CLASS_WEAPON)
        {
            handler->SendSysMessage(LANG_CMD_TRANSMOG_ADD_FORBIDDEN);
            handler->SetSentErrorMessage(true);
            return true;
        }

        auto guid = player->GetGUID();
        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
        uint32 itemId = itemTemplate->ItemId;

        std::stringstream tempStream;
        tempStream << std::hex << ItemQualityColors[itemTemplate->Quality];
        std::string itemQuality = tempStream.str();
        std::string itemName = itemTemplate->Name1;

        if (target) {
            // get locale item name
            int loc_idex = target->GetSession()->GetSessionDbLocaleIndex();
            if (ItemLocale const* il = sObjectMgr->GetItemLocale(itemId))
                ObjectMgr::GetLocaleString(il->Name, loc_idex, itemName);
        }

        std::string playerName = player->GetName();
        std::string nameLink = handler->playerLink(playerName);

        if (sTransmogrification->AddCollectedAppearance(accountId, itemId))
        {
            // Notify target of new item in appearance collection
            if (target && !(target->GetPlayerSetting("mod-transmog", SETTING_HIDE_TRANSMOG).value) && !sTransmogrification->CanNeverTransmog(itemTemplate))
                ChatHandler(target->GetSession()).PSendSysMessage(R"(|c{}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r has been added to your appearance collection.)", itemQuality.c_str(), itemId, itemName.c_str());

            // Feedback of successful command execution to GM
            if (isNotConsole && target != handler->GetPlayer())
                handler->PSendSysMessage(R"(|c{}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r has been added to the appearance collection of Player {}.)", itemQuality.c_str(), itemId, itemName.c_str(), nameLink);

            CharacterDatabase.Execute("INSERT INTO custom_unlocked_appearances (account_id, item_template_id) VALUES ({}, {})", accountId, itemId);
        }
        else
        {
            // Feedback of failed command execution to GM
            if (isNotConsole)
            {
                handler->PSendSysMessage(R"(Player {} already has item |c{}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r in the appearance collection.)", nameLink, itemQuality.c_str(), itemId, itemName.c_str());
                handler->SetSentErrorMessage(true);
            }
        }

        return true;
    }

    static bool HandleAddTransmogItemSet(ChatHandler* handler, Optional<PlayerIdentifier> player, Variant<Hyperlink<itemset>, uint32> itemSetId)
    {
        if (!sTransmogrification->GetUseCollectionSystem())
            return true;

        if (!*itemSetId)
        {
            handler->PSendSysMessage(LANG_NO_ITEMS_FROM_ITEMSET_FOUND, uint32(itemSetId));
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!player)
            return false;

        Player* target = player->GetConnectedPlayer();
        ItemSetEntry const* set = sItemSetStore.LookupEntry(uint32(itemSetId));
        bool isNotConsole = handler->GetSession();

        if (!set)
        {
            handler->PSendSysMessage(LANG_NO_ITEMS_FROM_ITEMSET_FOUND, uint32(itemSetId));
            handler->SetSentErrorMessage(true);
            return false;
        }

        auto guid = player->GetGUID();
        CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(guid);
        if (!playerData)
            return false;

        bool added = false;
        uint32 error = 0;
        uint32 itemId;
        uint32 accountId = playerData->AccountId;

        for (uint32 i = 0; i < MAX_ITEM_SET_ITEMS; ++i)
        {
            itemId = set->itemId[i];
            if (itemId)
            {
                ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemId);
                if (itemTemplate)
                {
                    if (!sTransmogrification->GetTrackUnusableItems() && (
                            (target && !sTransmogrification->SuitableForTransmogrification(target, itemTemplate)) ||
                            !sTransmogrification->SuitableForTransmogrification(guid, itemTemplate)
                            ))
                    {
                        error = LANG_CMD_TRANSMOG_ADD_UNSUITABLE;
                        continue;
                    }
                    if (itemTemplate->Class != ITEM_CLASS_ARMOR && itemTemplate->Class != ITEM_CLASS_WEAPON)
                    {
                        error = LANG_CMD_TRANSMOG_ADD_FORBIDDEN;
                        continue;
                    }

                    if (sTransmogrification->AddCollectedAppearance(accountId, itemId))
                    {
                        CharacterDatabase.Execute("INSERT INTO custom_unlocked_appearances (account_id, item_template_id) VALUES ({}, {})", accountId, itemId);
                        added = true;
                    }
                }
            }
        }

        if (!added && error > 0)
        {
            handler->SendSysMessage(error);
            handler->SetSentErrorMessage(true);
            return true;
        }

        int locale = handler->GetSessionDbcLocale();
        std::string setName = set->name[locale];
        std::string nameLink = handler->playerLink(player->GetName());

        // Feedback of command execution to GM
        if (isNotConsole)
        {
            // Failed command execution
            if (!added)
            {
                handler->PSendSysMessage("Player {} already has ItemSet |cffffffff|Hitemset:{}|h[{} {}]|h|r in the appearance collection.", nameLink, uint32(itemSetId), setName.c_str(), localeNames[locale]);
                handler->SetSentErrorMessage(true);
                return true;
            }

            // Successful command execution
            if (target != handler->GetPlayer())
                handler->PSendSysMessage("ItemSet |cffffffff|Hitemset:{}|h[{} {}]|h|r has been added to the appearance collection of Player {}.", uint32(itemSetId), setName.c_str(), localeNames[locale], nameLink);
        }

        // Notify target of new item in appearance collection
        if (target && !(target->GetPlayerSetting("mod-transmog", SETTING_HIDE_TRANSMOG).value))
            ChatHandler(target->GetSession()).PSendSysMessage("ItemSet |cffffffff|Hitemset:%d|h[{} {}]|h|r has been added to your appearance collection.", uint32(itemSetId), setName.c_str(), localeNames[locale]);

        return true;
    }

    static bool HandleTransmogPortableCommand(ChatHandler* handler)
    {
        if (!sTransmogrification->IsPortableNPCEnabled)
        {
            handler->SendErrorMessage("The portable transmogrification NPC is disabled.");
            return true;
        }

        if (!sTransmogrification->IsTransmogPlusEnabled)
        {
            handler->SendErrorMessage("The portable transmogrification NPC is a plus feature. Plus features are currently disabled.");
            return true;
        }

        Player* player = PlayerIdentifier::FromSelf(handler)->GetConnectedPlayer();

        if (!sTransmogrification->IsPlusFeatureEligible(player->GetGUID(), PLUS_FEATURE_PET))
        {
            handler->SendErrorMessage("You are not eligible for the portable transmogrification NPC. Please check your subscription level.");
            return true;
        }

        if (!sSpellMgr->GetSpellInfo(sTransmogrification->PetSpellId))
        {
            handler->SendErrorMessage("The portable transmogrification NPC spell is not available.");
            return true;
        }

        player->CastSpell((Unit*)nullptr, sTransmogrification->PetSpellId, true);
        return true;
    };

    static bool HandleInterfaceOption(ChatHandler* handler, bool enable)
    {
        handler->GetPlayer()->UpdatePlayerSetting("mod-transmog", SETTING_VENDOR_INTERFACE, enable);
        handler->SendSysMessage(enable ? LANG_CMD_TRANSMOG_VENDOR_INTERFACE_ENABLE : LANG_CMD_TRANSMOG_VENDOR_INTERFACE_DISABLE);
        return true;
    }
};

void AddSC_transmog_commandscript()
{
    new transmog_commandscript();
}
