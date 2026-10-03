#pragma once
// Son dans le casque. Le jeu joue son son avec FMOD Ex (DLL chargée dynamiquement), sur la
// sortie Windows par défaut au démarrage. On récupère son objet FMOD::System (détour de
// System::update, fonction exportée par la DLL FMOD) et on le bascule (System::setDriver)
// sur la sortie audio du casque :
//  - donnée par le runtime OpenXR quand il le permet (XR_OCULUS_audio_device_guid) ;
//  - sinon la sortie Windows par défaut, suivie (SteamVR, par exemple, y met le casque).
namespace engine {

bool InstallAudioRoute(); // false si FMOD n'est pas (encore) chargé : rappeler plus tard
void RemoveAudioRoute();

// Thread XR : sortie du casque (identifiant de périphérique Windows, chaîne vide = sortie
// par défaut). À rappeler régulièrement : la sortie par défaut peut changer.
void SetHeadsetAudioDevice(const wchar_t* endpointId);

} // namespace engine
