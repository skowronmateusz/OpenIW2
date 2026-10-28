# Engine built-in functions

Catalog of the script built-ins in `CoD2SP_s.exe` (1.3), read with REA from the executable's registration
tables. Each row is `{ const char* name; function; int developerOnly }` (12 bytes). The global function table
rows start at 0x5a5c20; the method tables are read the same way.
`dev` = 1 marks developer-only functions. *Used* means some shipped script calls the function.

163 functions and 295 methods; the scripts call 440 built-ins.

## Functions (called without an object)

| Name | Address | dev | Used |
|---|---|---|---|
| `print` | 0x4eb740 | 1 | yes |
| `println` | 0x4eb7d0 | 1 | yes |
| `iprintln` | 0x4ebc50 | 0 | yes |
| `iprintlnbold` | 0x4ebc60 | 0 | yes |
| `print3d` | 0x4ebc70 | 1 | yes |
| `line` | 0x4ebd70 | 1 | yes |
| `getent` | 0x4fa150 | 0 | yes |
| `getentarray` | 0x4fa290 | 0 | yes |
| `getnode` | 0x5162a0 | 0 | yes |
| `getnodearray` | 0x5163b0 | 0 | yes |
| `getallnodes` | 0x5164d0 | 0 | yes |
| `getvehiclenode` | 0x52b600 | 0 | yes |
| `getvehiclenodearray` | 0x52b700 | 0 | yes |
| `getallvehiclenodes` | 0x52b7e0 | 0 | yes |
| `getnumvehicles` | 0x5120d0 | 0 |  |
| `precachevehicle` | 0x512160 | 0 | yes |
| `precacheturret` | 0x4ed180 | 0 | yes |
| `spawn` | 0x4ece50 | 0 | yes |
| `spawnvehicle` | 0x4ecf50 | 0 | yes |
| `spawnturret` | 0x4ed0c0 | 0 | yes |
| `spawnstruct` | 0x46f080 | 0 | yes |
| `assert` | 0x4ebe60 | 1 | yes |
| `assertex` | 0x4ebe90 | 1 | yes |
| `assertmsg` | 0x4ebee0 | 1 | yes |
| `isdefined` | 0x4ebf20 | 0 | yes |
| `isstring` | 0x4ebfa0 | 0 | yes |
| `isalive` | 0x4ec000 | 0 | yes |
| `isplayer` | 0x4ec090 | 0 | yes |
| `isai` | 0x4ec100 | 0 | yes |
| `issentient` | 0x4ec170 | 0 | yes |
| `getcvar` | 0x4ec1e0 | 0 | yes |
| `getcvarint` | 0x4ec250 | 0 | yes |
| `getcvarfloat` | 0x4ec2d0 | 0 | yes |
| `getdebugcvar` | 0x4ec350 | 1 | yes |
| `getdebugcvarint` | 0x4ec3c0 | 1 | yes |
| `getdebugcvarfloat` | 0x4ec440 | 1 |  |
| `setcvar` | 0x4ec4c0 | 0 | yes |
| `setsavedcvar` | 0x4ec6b0 | 0 | yes |
| `gettime` | 0x4ec860 | 0 | yes |
| `getdifficulty` | 0x4ec870 | 0 | yes |
| `getentbynum` | 0x4ec890 | 1 |  |
| `getaiarray` | 0x4ec9f0 | 0 | yes |
| `getspawnerarray` | 0x4eca60 | 0 | yes |
| `getspawnerteamarray` | 0x4ecaf0 | 0 | yes |
| `getweaponmodel` | 0x4ecba0 | 0 | yes |
| `getanimlength` | 0x4ecc40 | 0 | yes |
| `animhasnotetrack` | 0x4eccb0 | 0 | yes |
| `getbrushmodelcenter` | 0x4ecd40 | 0 |  |
| `getkeybinding` | 0x4ecd60 | 0 | yes |
| `getcommandfromkey` | 0x4ece00 | 0 | yes |
| `objective_add` | 0x4f12d0 | 0 | yes |
| `objective_delete` | 0x4f16f0 | 0 | yes |
| `objective_state` | 0x4f1790 | 0 | yes |
| `objective_string` | 0x4f1b50 | 0 | yes |
| `objective_string_nomessage` | 0x4f1b60 | 0 | yes |
| `objective_icon` | 0x4f1b70 | 0 | yes |
| `objective_position` | 0x4f1c40 | 0 | yes |
| `objective_additionalposition` | 0x4f1d30 | 0 | yes |
| `objective_current` | 0x4f1e70 | 0 | yes |
| `objective_additionalcurrent` | 0x4f1ff0 | 0 | yes |
| `objective_ring` | 0x4f2140 | 0 | yes |
| `bullettrace` | 0x4f2230 | 0 | yes |
| `bullettracepassed` | 0x4f24c0 | 0 | yes |
| `sighttracepassed` | 0x4f2580 | 0 | yes |
| `physicstrace` | 0x4f2680 | 0 | yes |
| `getstartorigin` | 0x53be50 | 0 | yes |
| `getstartangles` | 0x53bf80 | 0 | yes |
| `getcycleoriginoffset` | 0x53c080 | 0 | yes |
| `getmovedelta` | 0x4f4810 | 0 | yes |
| `getangledelta` | 0x4f4920 | 0 |  |
| `getnorthyaw` | 0x4f4a30 | 0 | yes |
| `randomint` | 0x4f2720 | 0 | yes |
| `randomfloat` | 0x4f2780 | 0 | yes |
| `randomintrange` | 0x4f27c0 | 0 | yes |
| `randomfloatrange` | 0x4f2830 | 0 | yes |
| `sin` | 0x4f28d0 | 0 | yes |
| `cos` | 0x4f28f0 | 0 | yes |
| `tan` | 0x4f2910 | 0 |  |
| `asin` | 0x4f2990 | 0 |  |
| `acos` | 0x4f2a00 | 0 |  |
| `atan` | 0x4f2a70 | 0 |  |
| `int` | 0x4f2a90 | 0 | yes |
| `distance` | 0x4f2b70 | 0 | yes |
| `distancesquared` | 0x4f2bf0 | 0 | yes |
| `length` | 0x4f2c60 | 0 | yes |
| `lengthsquared` | 0x4f2ca0 | 0 | yes |
| `closer` | 0x4f2ce0 | 0 |  |
| `vectordot` | 0x4f2dc0 | 0 | yes |
| `vectornormalize` | 0x4f2e10 | 0 | yes |
| `vectortoangles` | 0x4f2e70 | 0 | yes |
| `anglestoup` | 0x4f2ec0 | 0 | yes |
| `anglestoright` | 0x4f2f20 | 0 | yes |
| `anglestoforward` | 0x4f3030 | 0 | yes |
| `issubstr` | 0x4f30e0 | 0 | yes |
| `getsubstr` | 0x4f3130 | 0 | yes |
| `tolower` | 0x4f31e0 | 0 | yes |
| `strtok` | 0x4f3260 | 0 | yes |
| `setblur` | 0x4f3340 | 0 | yes |
| `musicplay` | 0x4f3400 | 0 | yes |
| `musicstop` | 0x4f3470 | 0 | yes |
| `soundfade` | 0x4f3520 | 0 |  |
| `ambientplay` | 0x4f3800 | 0 | yes |
| `ambientstop` | 0x4f3930 | 0 |  |
| `precachemodel` | 0x4f3580 | 0 | yes |
| `precacheshellshock` | 0x4f35e0 | 0 | yes |
| `precacheitem` | 0x4f3640 | 0 | yes |
| `precacheshader` | 0x4f36f0 | 0 | yes |
| `precachestring` | 0x4f3770 | 0 | yes |
| `precachemenu` | 0x4f8860 | 0 | yes |
| `precacherumble` | 0x4f37f0 | 0 | yes |
| `savegame` | 0x4f3a30 | 0 | yes |
| `issavesuccessful` | 0x4f3d80 | 0 | yes |
| `issaverecentlyloaded` | 0x4f3da0 | 0 | yes |
| `savegamenocommit` | 0x4f3bd0 | 0 | yes |
| `commitsave` | 0x4f3db0 | 0 | yes |
| `loadfx` | 0x4f4a80 | 0 | yes |
| `playfx` | 0x4f4be0 | 0 | yes |
| `playfxontag` | 0x4f4eb0 | 0 | yes |
| `playloopedfx` | 0x4f50c0 | 0 | yes |
| `getfxvisibility` | 0x4f53b0 | 0 | yes |
| `setcullfog` | 0x4f5580 | 0 | yes |
| `setexpfog` | 0x4f5630 | 0 | yes |
| `setculldist` | 0x4f5700 | 0 | yes (far plane, render-hints.md) |
| `setsunlight` | 0x4f5750 | 0 | yes |
| `resetsunlight` | 0x4f57c0 | 0 | yes |
| `radiusdamage` | 0x4f3e70 | 0 | yes |
| `setplayerignoreradiusdamage` | 0x4f3ef0 | 0 | yes |
| `changelevel` | 0x4f3f00 | 0 | yes |
| `missionsuccess` | 0x4f4040 | 0 | yes |
| `missionfailed` | 0x4f4080 | 0 | yes |
| `setmissioncvar` | 0x4f40b0 | 0 | yes |
| `cinematic` | 0x4f4220 | 0 | yes |
| `earthquake` | 0x4f4330 | 0 | yes |
| `drawcompassfriendlies` | 0x4f4450 | 0 |  |
| `bulletspread` | 0x4ee760 | 0 | yes |
| `bullettracer` | 0x4ee830 | 0 | yes |
| `magicbullet` | 0x4ee880 | 0 | yes |
| `getnumparts` | 0x4f5b60 | 0 |  |
| `getpartname` | 0x4f5bc0 | 0 | yes |
| `weaponfiretime` | 0x4f4460 | 0 |  |
| `weaponclipsize` | 0x4f44d0 | 0 |  |
| `weaponissemiauto` | 0x4f45d0 | 0 |  |
| `weaponisboltaction` | 0x4f4630 | 0 |  |
| `weapontype` | 0x4f4690 | 0 |  |
| `weaponfightdist` | 0x4f4770 | 0 | yes |
| `weaponmaxdist` | 0x4f4790 | 0 |  |
| `isturretactive` | 0x4f47b0 | 0 | yes |
| `badplace_delete` | 0x4f5800 | 0 | yes |
| `badplace_cylinder` | 0x4f5880 | 0 | yes |
| `badplace_arc` | 0x4f5990 | 0 | yes |
| `newhudelem` | 0x5300a0 | 0 | yes |
| `resettimeout` | 0x46f600 | 0 | yes |
| `setturretnode` | 0x515910 | 0 | yes |
| `unsetturretnode` | 0x5159a0 | 0 | yes |
| `setnodepriority` | 0x515810 | 0 |  |
| `isnodeoccupied` | 0x5158c0 | 0 |  |
| `setdebugorigin` | 0x4f8ac0 | 1 | yes |
| `setdebugangles` | 0x4f8b00 | 1 | yes |
| `playrumbleonpos` | 0x4f8b20 | 0 | yes |
| `playlooprumbleonpos` | 0x4f8b30 | 0 |  |
| `stopallrumbles` | 0x4f8b40 | 0 |  |
| `soundexists` | 0x4ef380 | 0 | yes |
| `giveachievement` | 0x4f8b50 | 0 | yes |

## Methods (called on an entity: `ent name(...)`)

| Name | Address | dev | Used |
|---|---|---|---|
| `animscripted` | 0x4f6240 | 0 | yes |
| `stopanimscripted` | 0x4f61e0 | 0 | yes |
| `attach` | 0x4ed1e0 | 0 | yes |
| `detach` | 0x4ed300 | 0 | yes |
| `detachall` | 0x4ed470 | 0 | yes |
| `getattachsize` | 0x4ed4d0 | 0 | yes |
| `getattachmodelname` | 0x4ed580 | 0 | yes |
| `getattachtagname` | 0x4ed630 | 0 | yes |
| `getattachignorecollision` | 0x4ed6c0 | 0 | yes |
| `linkto` | 0x4ed770 | 0 | yes |
| `setstablemissile` | 0x4efde0 | 0 | yes |
| `playerlinktodelta` | 0x4eda80 | 0 | yes |
| `playerlinktoabsolute` | 0x4edc60 | 0 | yes |
| `dontinterpolate` | 0x4edf90 | 0 | yes |
| `unlink` | 0x4ede30 | 0 | yes |
| `enablelinkto` | 0x4edeb0 | 0 | yes |
| `dospawn` | 0x4edff0 | 0 | yes |
| `stalingradspawn` | 0x4ee120 | 0 | yes |
| `getorigin` | 0x4ee250 | 0 | yes |
| `geteye` | 0x4eee60 | 0 | yes |
| `getdebugeye` | 0x4eef50 | 1 | yes |
| `useby` | 0x4ef020 | 0 | yes |
| `istouching` | 0x4ef100 | 0 | yes |
| `playsound` | 0x4ef3b0 | 0 | yes |
| `playsoundasmaster` | 0x4ef3d0 | 0 | yes |
| `playloopsound` | 0x4ef3f0 | 0 | yes |
| `stoploopsound` | 0x4ef4c0 | 0 | yes |
| `playrumble` | 0x4f8bf0 | 0 | yes |
| `playlooprumble` | 0x4f8c00 | 0 | yes |
| `stoprumble` | 0x4f8c10 | 0 | yes |
| `iswaitingonsound` | 0x4ef510 | 0 |  |
| `delete` | 0x4ef570 | 0 | yes |
| `setmodel` | 0x4ef620 | 0 | yes |
| `setshadowhint` | 0x4ef700 | 0 | yes |
| `getnormalhealth` | 0x4ef830 | 0 | yes |
| `setnormalhealth` | 0x4ef8f0 | 0 | yes |
| `dodamage` | 0x4efaf0 | 0 | yes |
| `show` | 0x4efcb0 | 0 | yes |
| `hide` | 0x4efd10 | 0 | yes |
| `setcontents` | 0x4efd70 | 0 | yes |
| `disconnectpaths` | 0x4efe80 | 0 | yes |
| `connectpaths` | 0x4eff40 | 0 | yes |
| `startfiring` | 0x4f0000 | 0 | yes |
| `stopfiring` | 0x4f00a0 | 0 | yes |
| `shootturret` | 0x4f0140 | 0 | yes |
| `setmode` | 0x4f01f0 | 0 | yes |
| `getturretowner` | 0x4f0300 | 0 | yes |
| `settargetentity` | 0x4f03c0 | 0 | yes |
| `setplayerspread` | 0x4f0550 | 0 |  |
| `setaispread` | 0x4f04b0 | 0 | yes |
| `setconvergencetime` | 0x4f05f0 | 0 | yes |
| `setsuppressiontime` | 0x4f0710 | 0 |  |
| `cleartargetentity` | 0x4f07c0 | 0 | yes |
| `setturretteam` | 0x4f0870 | 0 | yes |
| `maketurretusable` | 0x4f0a40 | 0 | yes |
| `maketurretunusable` | 0x4f0ae0 | 0 | yes |
| `setturretaccuracy` | 0x4f0b80 | 0 |  |
| `setturretignoregoals` | 0x4f0980 | 0 | yes |
| `getturrettarget` | 0x4f0b90 | 0 | yes |
| `setcursorhint` | 0x4f0c50 | 0 | yes |
| `sethintstring` | 0x4f0ec0 | 0 | yes |
| `clearanim` | 0x4f6300 | 0 | yes |
| `setanimknob` | 0x4f6640 | 0 | yes |
| `setanimknobrestart` | 0x4f6650 | 0 | yes |
| `setanimknoball` | 0x4f6910 | 0 | yes |
| `setanimknoballrestart` | 0x4f6920 | 0 | yes |
| `setanim` | 0x4f6b70 | 0 | yes |
| `setanimrestart` | 0x4f6b80 | 0 | yes |
| `getanimtime` | 0x4f6b90 | 0 | yes |
| `setflaggedanimknob` | 0x4f6f20 | 0 | yes |
| `setflaggedanimknobrestart` | 0x4f6f30 | 0 | yes |
| `setflaggedanimknoball` | 0x4f7250 | 0 | yes |
| `setflaggedanimknoballrestart` | 0x4f7270 | 0 | yes |
| `setflaggedanim` | 0x4f7520 | 0 | yes |
| `setflaggedanimrestart` | 0x4f7530 | 0 | yes |
| `useanimtree` | 0x4f7600 | 0 | yes |
| `stopuseanimtree` | 0x4f76e0 | 0 | yes |
| `dumpanims` | 0x4f77d0 | 0 |  |
| `getstance` | 0x4ee330 | 0 | yes |
| `getammocount` | 0x4f4540 | 0 | yes |
| `magicgrenade` | 0x4ee3d0 | 0 | yes |
| `magicgrenademanual` | 0x4ee5e0 | 0 | yes |
| `isfiringturret` | 0x4eea60 | 0 | yes |
| `setfriendlychain` | 0x4eeb10 | 0 | yes |
| `gettagorigin` | 0x4eee10 | 0 | yes |
| `gettagangles` | 0x4eee30 | 0 | yes |
| `shellshock` | 0x4f7820 | 0 | yes |
| `stopshellshock` | 0x4f79d0 | 0 | yes |
| `viewkick` | 0x4f7a40 | 0 | yes |
| `getentnum` | 0x4f7b30 | 1 | yes |
| `locklightvis` | 0x4f7b40 | 0 |  |
| `unlocklightvis` | 0x4f7b90 | 0 |  |
| `launch` | 0x4f7be0 | 0 | yes |
| `setsoundblend` | 0x4f7d10 | 0 | yes |
| `localtoworldcoords` | 0x4f7e60 | 0 | yes |
| `getentitynumber` | 0x4f7fa0 | 0 | yes |
| `enablegrenadetouchdamage` | 0x4f8000 | 0 | yes |
| `disablegrenadetouchdamage` | 0x4f8080 | 0 |  |
| `enablegrenadebounce` | 0x4f8100 | 0 |  |
| `disablegrenadebounce` | 0x4f8160 | 0 |  |
| `enableaimassist` | 0x4f81c0 | 0 | yes |
| `disableaimassist` | 0x4f8230 | 0 | yes |
| `makefakeai` | 0x4f82a0 | 0 | yes |
| `setlookattext` | 0x4f8370 | 0 | yes |
| `setspawnerteam` | 0x4f8750 | 0 | yes |
| `setrightarc` | 0x4f8440 | 0 | yes |
| `setleftarc` | 0x4f84d0 | 0 | yes |
| `settoparc` | 0x4f8560 | 0 | yes |
| `setbottomarc` | 0x4f85f0 | 0 | yes |
| `setdefaultdroppitch` | 0x4f8680 | 0 | yes |
| `restoredefaultdroppitch` | 0x4f86f0 | 0 | yes |
| `attachpath` | 0x513070 | 0 | yes |
| `startpath` | 0x513220 | 0 | yes |
| `setswitchnode` | 0x5132a0 | 0 | yes |
| `setwaitnode` | 0x513370 | 0 | yes |
| `setwaitspeed` | 0x513400 | 0 | yes |
| `setspeed` | 0x513490 | 0 | yes |
| `getspeed` | 0x513570 | 0 | yes |
| `getspeedmph` | 0x5135e0 | 0 | yes |
| `resumespeed` | 0x513640 | 0 | yes |
| `joltbody` | 0x5136e0 | 0 | yes |
| `freevehicle` | 0x513850 | 0 | yes |
| `getwheelsurface` | 0x513920 | 0 | yes |
| `getvehicleowner` | 0x513ab0 | 0 | yes |
| `startenginesound` | 0x513e80 | 0 | yes |
| `stopenginesound` | 0x513ee0 | 0 | yes |
| `makevehicleusable` | 0x513b20 | 0 | yes |
| `makevehicleunusable` | 0x513b80 | 0 | yes |
| `addvehicletocompass` | 0x513c10 | 0 | yes |
| `removevehiclefromcompass` | 0x513c70 | 0 | yes |
| `setvehiclelookattext` | 0x513cd0 | 0 | yes |
| `setvehicleteam` | 0x513da0 | 0 | yes |
| `setturrettargetvec` | 0x513f40 | 0 | yes |
| `setturrettargetent` | 0x514050 | 0 | yes |
| `clearturrettarget` | 0x5141b0 | 0 | yes |
| `fireturret` | 0x514260 | 0 | yes |
| `isturretready` | 0x5148e0 | 0 | yes |
| `settext` | 0x5301a0 | 0 | yes |
| `setshader` | 0x530280 | 0 | yes |
| `settimer` | 0x530770 | 0 | yes |
| `settimerup` | 0x530780 | 0 |  |
| `settenthstimer` | 0x530790 | 0 | yes |
| `settenthstimerup` | 0x5307a0 | 0 |  |
| `setclock` | 0x5307b0 | 0 | yes |
| `setclockup` | 0x5307c0 | 0 |  |
| `setvalue` | 0x5307d0 | 0 | yes |
| `fadeovertime` | 0x530860 | 0 | yes |
| `scaleovertime` | 0x530960 | 0 | yes |
| `moveovertime` | 0x530aa0 | 0 | yes |
| `reset` | 0x530bb0 | 0 |  |
| `destroy` | 0x530c00 | 0 | yes |
| `moveto` | 0x533610 | 0 | yes |
| `movex` | 0x533900 | 0 |  |
| `movey` | 0x533910 | 0 |  |
| `movez` | 0x533920 | 0 | yes |
| `movegravity` | 0x5336e0 | 0 | yes |
| `rotateto` | 0x533930 | 0 | yes |
| `rotatepitch` | 0x533b60 | 0 | yes |
| `rotateyaw` | 0x533b70 | 0 | yes |
| `rotateroll` | 0x533b80 | 0 | yes |
| `vibrate` | 0x533b90 | 0 | yes |
| `rotatevelocity` | 0x533db0 | 0 | yes |
| `solid` | 0x533f70 | 0 | yes |
| `notsolid` | 0x534050 | 0 | yes |
| `setcandamage` | 0x533eb0 | 0 | yes |
| `giveweapon` | 0x5376d0 | 0 | yes |
| `takeweapon` | 0x537850 | 0 | yes |
| `takeallweapons` | 0x537960 | 0 | yes |
| `getcurrentweapon` | 0x537a50 | 0 | yes |
| `getcurrentoffhand` | 0x537b00 | 0 | yes |
| `hasweapon` | 0x537bb0 | 0 |  |
| `switchtoweapon` | 0x537c90 | 0 | yes |
| `switchtooffhand` | 0x537dd0 | 0 | yes |
| `givestartammo` | 0x537f10 | 0 |  |
| `givemaxammo` | 0x538020 | 0 | yes |
| `getfractionstartammo` | 0x538130 | 0 |  |
| `getfractionmaxammo` | 0x538270 | 0 | yes |
| `setorigin` | 0x538410 | 0 | yes |
| `setvelocity` | 0x5384b0 | 0 |  |
| `setplayerangles` | 0x538560 | 0 | yes |
| `getplayerangles` | 0x538600 | 0 | yes |
| `getnormalizedmovement` | 0x5386a0 | 0 | yes |
| `usebuttonpressed` | 0x538770 | 0 | yes |
| `attackbuttonpressed` | 0x538820 | 0 | yes |
| `meleebuttonpressed` | 0x5388d0 | 0 | yes |
| `buttonpressed` | 0x538980 | 0 | yes |
| `playerads` | 0x538a60 | 0 | yes |
| `isonground` | 0x538b00 | 0 | yes |
| `setviewmodel` | 0x538bb0 | 0 | yes |
| `allowstand` | 0x538c90 | 0 | yes |
| `allowcrouch` | 0x538d40 | 0 | yes |
| `allowprone` | 0x538df0 | 0 | yes |
| `allowleanleft` | 0x538ea0 | 0 | yes |
| `allowleanright` | 0x538f50 | 0 | yes |
| `openmenu` | 0x539000 | 0 | yes |
| `openmenunomouse` | 0x539110 | 0 |  |
| `closemenu` | 0x539220 | 0 | yes |
| `freezecontrols` | 0x5392e0 | 0 | yes |
| `setreverb` | 0x539380 | 0 | yes |
| `deactivatereverb` | 0x539560 | 0 | yes |
| `setchannelvolumes` | 0x5396b0 | 0 |  |
| `deactivatechannelvolumes` | 0x539850 | 0 |  |
| `islookingat` | 0x5399b0 | 0 | yes |
| `isthrowinggrenade` | 0x539b60 | 0 | yes |
| `isfiring` | 0x539aa0 | 0 | yes |
| `ismeleeing` | 0x539c10 | 0 | yes |
| `playlocalsound` | 0x539cc0 | 0 | yes |
| `setautopickup` | 0x53a780 | 0 |  |
| `getweaponslotweapon` | 0x539df0 | 0 | yes |
| `setweaponslotweapon` | 0x539f40 | 0 | yes |
| `getweaponslotammo` | 0x53a1c0 | 0 | yes |
| `setweaponslotammo` | 0x53a320 | 0 | yes |
| `getweaponslotclipammo` | 0x53a4d0 | 0 | yes |
| `setweaponslotclipammo` | 0x53a620 | 0 | yes |
| `setweaponclipammo` | 0x53a830 | 0 | yes |
| `enablehealthshield` | 0x53a940 | 0 | yes |
| `setclientdvar` | 0x53a9f0 | 0 |  |
| `getenemysqdist` | 0x53c230 | 0 |  |
| `getclosestenemysqdist` | 0x53c290 | 0 | yes |
| `startscriptedanim` | 0x53c520 | 0 | yes |
| `shoot` | 0x53c5a0 | 0 |  |
| `melee` | 0x53c690 | 0 | yes |
| `findcovernode` | 0x53c6e0 | 0 |  |
| `findbestcovernode` | 0x53c730 | 0 | yes |
| `getcovernode` | 0x53c880 | 0 |  |
| `usecovernode` | 0x53c900 | 0 | yes |
| `reacquirestep` | 0x53c9c0 | 0 | yes |
| `findreacquirenode` | 0x53ca30 | 0 | yes |
| `getreacquirenode` | 0x53cac0 | 0 | yes |
| `usereacquirenode` | 0x53cb40 | 0 | yes |
| `findreacquiredirectpath` | 0x53cc00 | 0 | yes |
| `trimpathtoattack` | 0x53cc50 | 0 | yes |
| `reacquiremove` | 0x53ccb0 | 0 | yes |
| `findreacquireproximatepath` | 0x53cd40 | 0 | yes |
| `flagenemyunattackable` | 0x53cd90 | 0 | yes |
| `setaimanims` | 0x53ce20 | 0 | yes |
| `aimatpos` | 0x53cf00 | 0 | yes |
| `enterprone` | 0x53d1b0 | 0 | yes |
| `exitprone` | 0x53d280 | 0 | yes |
| `setproneanimnodes` | 0x53d2f0 | 0 | yes |
| `updateprone` | 0x53d430 | 0 | yes |
| `setlookatanimnodes` | 0x53d590 | 0 |  |
| `setlookat` | 0x53d630 | 0 |  |
| `setlookatyawlimits` | 0x53d6f0 | 0 |  |
| `stoplookat` | 0x53d770 | 0 |  |
| `canshoot` | 0x53d800 | 0 | yes |
| `cansee` | 0x53d930 | 0 | yes |
| `dropweapon` | 0x53da50 | 0 |  |
| `maymovetopoint` | 0x53dce0 | 0 | yes |
| `teleport` | 0x53dfb0 | 0 | yes |
| `withinapproxpathdist` | 0x53e2d0 | 0 | yes |
| `ispathdirect` | 0x53e350 | 0 |  |
| `allowedstances` | 0x53e3c0 | 0 | yes |
| `isstanceallowed` | 0x53e550 | 0 | yes |
| `issuppressed` | 0x53e6a0 | 0 | yes |
| `checkgrenadethrow` | 0x53e720 | 0 | yes |
| `throwgrenade` | 0x53ee10 | 0 | yes |
| `pickupgrenade` | 0x53f0a0 | 0 | yes |
| `useturret` | 0x53f100 | 0 | yes |
| `stopuseturret` | 0x53f200 | 0 | yes |
| `canuseturret` | 0x53f280 | 0 | yes |
| `traversemode` | 0x53f360 | 0 | yes |
| `animmode` | 0x53f410 | 0 | yes |
| `orientmode` | 0x53f510 | 0 | yes |
| `getmotionangle` | 0x53f770 | 0 | yes |
| `setturretanim` | 0x53f860 | 0 | yes |
| `getturret` | 0x53f8e0 | 0 | yes |
| `beginprediction` | 0x53f960 | 0 | yes |
| `endprediction` | 0x53fb80 | 0 | yes |
| `lerpposition` | 0x53fdb0 | 0 | yes |
| `predictoriginandangles` | 0x53ff50 | 0 | yes |
| `predictanim` | 0x540020 | 0 | yes |
| `gethitenttype` | 0x5400c0 | 0 | yes |
| `gethityaw` | 0x540110 | 0 | yes |
| `getgroundenttype` | 0x5401c0 | 0 | yes |
| `isdeflected` | 0x540220 | 0 | yes |
| `trackscriptstate` | 0x540280 | 0 | yes |
| `dumphistory` | 0x540400 | 0 |  |
| `animcustom` | 0x540450 | 0 | yes |
| `canattackenemynode` | 0x540500 | 0 | yes |
| `getnegotiationstartnode` | 0x5405b0 | 0 | yes |
| `getnegotiationendnode` | 0x540650 | 0 | yes |
| `checkprone` | 0x5406f0 | 0 | yes |
| `pushplayer` | 0x5407a0 | 0 | yes |
| `checkgrenadethrowpos` | 0x53ead0 | 0 | yes |
| `setgoalnode` | 0x540880 | 0 | yes |
| `setgoalpos` | 0x540940 | 0 | yes |
| `setgoalentity` | 0x5409f0 | 0 | yes |
| `setgoalvolume` | 0x540aa0 | 0 | yes |
| `getgoalvolume` | 0x540b90 | 0 |  |
| `isingoal` | 0x540c00 | 0 | yes |
| `nearnode` | 0x540ca0 | 0 | yes |
| `clearenemy` | 0x540d10 | 0 |  |
| `setpotentialthreat` | 0x540dd0 | 0 | yes |
| `clearpotentialthreat` | 0x540e70 | 0 |  |

## Not found in the tables above

These are called by scripts but are compiled specially (`notify`, `waittill`, `waittillmatch`, `endon`) or live in tables not yet located: `addtestclient`, `allowspectateteam`, `clientclaimtrigger`, `clientreleasetrigger`, `cloneplayer`, `closeingamemenu`, `disableweapon`, `dropitem`, `enableweapon`, `endon`, `endparty`, `exitlevel`, `finishplayerdamage`, `getguid`, `getteamscore`, `getviewmodel`, `issplitscreen`, `logprint`, `makecvarserverinfo`, `map_restart`, `newclienthudelem`, `newteamhudelem`, `notify`, `obituary`, `objective_onentity`, `objective_team`, `pingplayer`, `placespawnpoint`, `positionwouldtelefrag`, `precacheheadicon`, `precachestatusicon`, `prof_begin`, `prof_end`, `sayall`, `sayteam`, `sendranks`, `setarchive`, `setclientcvar`, `setclientnamemode`, `setplayerteamrank`, `setspawnweapon`, `setteamfortrigger`, `setteamscore`, `setwaypoint`, `suicide`, `updatescores`, `waittill`, `waittillmatch`.

## Proven no-ops (single `ret` or no handler)

Read from the executable bytes (`c3 cc cc cc` at each address): `playrumble` 0x4f8bf0, `stoprumble` 0x4f8c10, `playrumbleonpos` 0x4f8b20,
`giveachievement` 0x4f8b50. `bullettracer` (0x4ee830) allocates a temp entity of event 0xb2 (type 0xc1) holding a vector at +0x64, but the
client event switch (0x4cbea9, event - 9, byte table 0x4cc04c, jump table 0x4cc02c) sends index 0xb8 (byte 7) to the default exit 0x4cc023, so
nothing reads it. All five are registered as empty built-ins.

## Implementation audit (counting stubs)

`iw2game <main> <map> --stubs` lists every name the shipped scripts call that has no implementation; `World::registerMissingStubs`
registers those as counting no-ops. Per-file call counts below come from the shipped .gsc files (SP = outside `maps/mp`).

Implemented from the exe: `attackbuttonpressed` (0x538820, buttons & 1), `usebuttonpressed` (0x538770, & 0x28), `getplayerangles`
(0x538600), `getnormalizedmovement` (0x5386a0, signed bytes x 1/127), `getammocount` (0x4f4540, stock + clip), `setspawnerteam`
(0x4f8750; all 100 shipped aitype `spawner()` calls name the team of their class name, so `getspawnerteamarray` already agreed),
`playlooprumble` (0x4f8c00, a single `ret`).

Still stubbed and called by SP scripts (most used first). Engine syntax/VM ops (`waittill`, `endon`, `notify`, `waittillmatch`) are
handled by the VM, not as built-ins; `line` / `print3d` (99 / 77 calls, developer-only drawing) are intentionally no-ops.

| Name | SP files | Where | Notes |
|---|---|---|---|
| `predictanim`, `predictoriginandangles`, `lerpposition`, `gethityaw`, `isdeflected`, `beginprediction`, `endprediction` | 1-2 | animscripts/predict.gsc, death.gsc | balcony stumble/tumble death prediction (anim playback against world collision) |
| `getgroundenttype` | 2 | animscripts/death.gsc, traverse/window.gsc | exe 0x5401c0 |
| `getcommandfromkey` | 1 | moscow_str | key-binding text (table 0x5d2730) for hint strings |
| `buttonpressed` | 1 | _briefing | key-name keyed query (binding table 0x5d2728) |
| `setstablemissile` | 1 | crossroads | sets entity flag 0x20000 (+0x128); consumer not traced |
| `enablegrenadetouchdamage` | 1 | tankhunt | sets entity flag 0x4000 (+0x128); consumer not traced |
| `setdebugorigin`, `setdebugangles`, `getdebugeye` | 1 | _load, utility | developer only |
| `getviewmodel`, `isonground` | 0-1 | | |

The remaining names (team scores, ranks, `logprint`, `obituary`, `closeingamemenu`, `setclientcvar`, ...) are only called from
`maps/mp` / gametype scripts.
