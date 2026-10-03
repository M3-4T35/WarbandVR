#pragma once

namespace engine {

// Champ de texte du jeu (nom du personnage, du royaume...) : le moteur lève un drapeau de
// saisie quand une zone de texte prend le focus (rglTextBoxWidget::obtainFocus). Trouvé par
// signature ; si elle manque, TextInputActive() renvoie toujours false (loggué).
bool InitTextInput();
bool TextInputActive();

} // namespace engine
