#pragma once

namespace engine {

// Cadence du jeu. Le moteur limite ses images avec un temps minimal par image
// (CD3DApplication::SetMinFrameTime, réglé d'après iMaxFrameRate au démarrage). En rendu
// alterné, chaque image ne sert qu'un œil : pour une cadence régulière, le jeu doit produire
// un multiple exact de la fréquence du casque. Trouvé par signature ; sans effet sinon.
bool InitFramePacing();
// Images par seconde maximales du jeu (tout thread)
bool SetGameMaxFps(double fps);

} // namespace engine
