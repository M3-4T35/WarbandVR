#pragma once
#include <string>

namespace engine {

// Menu d'ordres du moteur (F1-F8, en haut à gauche en bataille) : niveau courant et libellés,
// tels que le jeu les affiche (traduits, propres au module). Détour de
// mbTacticalWindow::updateOrdersMenu, trouvé par signature ; désactivé proprement sinon.
bool InstallOrdersHook();
void RemoveOrdersHook();

struct OrderMenu {
    bool open = false;      // panneau d'ordres affiché
    int type = -2;          // niveau du menu (-1 = racine)
    int count = 0;          // nombre d'entrées (8 au plus)
    std::string labels[8];  // UTF-8
};
// Lisible depuis n'importe quel thread
OrderMenu GetOrderMenu();

} // namespace engine
