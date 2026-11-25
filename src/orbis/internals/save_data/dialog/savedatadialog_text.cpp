// SPDX-License-Identifier: GPL-2.0-or-later
#include "savedatadialog_text.h"

#include <algorithm>
#include <array>

namespace Libraries::SaveData::Dialog {
namespace {

constexpr SaveText spanish{
    "GUARDAR PARTIDA", "CARGAR PARTIDA", "ELIMINAR PARTIDA", "DUPLICAR PARTIDA", "SOBRESCRIBIR PARTIDA", "Crear nueva partida guardada",
    "Espacio vacío", "No hay partidas guardadas.", "Guardado automático", "Guardado manual", "Partida dañada",
    "Dificultad", "Tiempo de juego", "Fecha de guardado", "Tamaño", "Carpeta", "Estado", "{}/64 PARTIDAS",
    "DUPLICAR", "ELIMINAR", "CREAR", "SELECCIONAR", "VOLVER", "ABRIR CARPETA", "CONFIRMAR", "Sí", "No", "Aceptar", "Cancelar",
    "¿Quieres guardar la partida?", "¿Quieres cargar esta partida?", "¿Quieres eliminar esta partida?",
    "¿Quieres duplicar esta partida?", "¿Quieres sobrescribir esta partida?",
    "Guardando…", "Cargando…", "Eliminando…", "Restaurando datos guardados…",
    "Partida guardada.", "Partida cargada.", "Partida eliminada.",
    "Los datos guardados están dañados.", "Los datos guardados están dañados y se eliminarán.",
    "Los datos guardados están dañados. Se eliminarán y se crearán otros nuevos.",
    "Los datos guardados están dañados. Se restaurará la copia de seguridad.",
    "No hay espacio suficiente para guardar. Se necesitan {} libres.", "No se pueden crear más partidas guardadas.",
    "Se ha producido un error. ({:X})", "Tipo de mensaje desconocido: {}", "ERROR AL CARGAR",
    "No se pudo guardar la partida.", "No se pudo cargar la partida.", "No se pudo eliminar la partida.",
    "No se pudo duplicar la partida.",
};

constexpr std::array<SaveText, static_cast<size_t>(Gow3::UI::GameLanguage::Count)> texts{{
    { // EN, UK
        "SAVE GAME", "LOAD GAME", "DELETE SAVE", "DUPLICATE SAVE", "OVERWRITE SAVE", "Create a New Save",
        "Empty slot", "No saved data", "Auto Save", "Manual Save", "Corrupted Save File",
        "Difficulty", "Play Time", "Date Saved", "File Size", "Folder", "Condition", "{}/64 SAVES",
        "DUPLICATE", "DELETE", "CREATE", "SELECT", "BACK", "OPEN SAVE FOLDER", "CONFIRM", "Yes", "No", "OK", "Cancel",
        "Do you want to save?", "Do you want to load this saved data?", "Do you want to delete this saved data?",
        "Do you wish to duplicate the save?", "Are you sure you want to overwrite this save?",
        "Saving...", "Loading...", "Deleting...", "Restoring saved data...",
        "Saved successfully.", "Loading complete.", "Deletion complete.",
        "The saved data is corrupted.", "The saved data is corrupted and will be deleted.",
        "The saved data is corrupted. This saved data will be deleted and a new one will be created.",
        "The saved data is corrupted. The data that was backed up by the system will be restored.",
        "There is not enough space to save the data. {} free space is required.", "Cannot create more saved data.",
        "An error has occurred. ({:X})", "Unknown message type: {}", "LOADING FAILED", "The saved data could not be saved.",
        "The saved data could not be loaded.", "The saved data could not be deleted.", "The saved data could not be duplicated.",
    },
    { // BR
        "SALVAR JOGO", "CARREGAR JOGO", "EXCLUIR JOGO SALVO", "DUPLICAR JOGO SALVO", "SOBRESCREVER JOGO SALVO", "Criar novo jogo salvo",
        "Espaço vazio", "Nenhum jogo salvo", "Salvamento automático", "Salvamento manual", "Jogo salvo corrompido",
        "Dificuldade", "Tempo de jogo", "Data do salvamento", "Tamanho", "Pasta", "Estado", "{}/64 JOGOS SALVOS",
        "DUPLICAR", "EXCLUIR", "CRIAR", "SELECIONAR", "VOLTAR", "ABRIR PASTA DO JOGO SALVO", "CONFIRMAR", "Sim", "Não", "OK", "Cancelar",
        "Deseja salvar o jogo?", "Deseja carregar este jogo salvo?", "Deseja excluir este jogo salvo?",
        "Deseja duplicar este jogo salvo?", "Deseja sobrescrever este jogo salvo?",
        "Salvando…", "Carregando…", "Excluindo…", "Restaurando dados salvos…",
        "Jogo salvo.", "Carregamento concluído.", "Exclusão concluída.",
        "Os dados salvos estão corrompidos.", "Os dados salvos estão corrompidos e serão excluídos.",
        "Os dados salvos estão corrompidos. Eles serão excluídos e novos dados serão criados.",
        "Os dados salvos estão corrompidos. A cópia de segurança será restaurada.",
        "Não há espaço suficiente para salvar. São necessários {} livres.", "Não é possível criar mais jogos salvos.",
        "Ocorreu um erro. ({:X})", "Tipo de mensagem desconhecido: {}", "FALHA AO CARREGAR",
        "Não foi possível salvar o jogo.", "Não foi possível carregar o jogo salvo.", "Não foi possível excluir o jogo salvo.",
        "Não foi possível duplicar o jogo salvo.",
    },
    { // DE
        "SPIEL SPEICHERN", "SPIEL LADEN", "SPIELSTAND LÖSCHEN", "SPIELSTAND DUPLIZIEREN", "SPIELSTAND ÜBERSCHREIBEN", "Neuen Spielstand erstellen",
        "Leerer Speicherplatz", "Keine Spielstände vorhanden", "Automatische Speicherung", "Manuelle Speicherung", "Beschädigter Spielstand",
        "Schwierigkeit", "Spielzeit", "Speicherdatum", "Dateigröße", "Ordner", "Zustand", "{}/64 SPIELSTÄNDE",
        "DUPLIZIEREN", "LÖSCHEN", "ERSTELLEN", "AUSWÄHLEN", "ZURÜCK", "SPEICHERORDNER ÖFFNEN", "BESTÄTIGEN", "Ja", "Nein", "OK", "Abbrechen",
        "Möchtest du speichern?", "Möchtest du diesen Spielstand laden?", "Möchtest du diesen Spielstand löschen?",
        "Möchtest du diesen Spielstand duplizieren?", "Möchtest du diesen Spielstand überschreiben?",
        "Wird gespeichert…", "Wird geladen…", "Wird gelöscht…", "Spielstand wird wiederhergestellt…",
        "Erfolgreich gespeichert.", "Laden abgeschlossen.", "Löschen abgeschlossen.",
        "Der Spielstand ist beschädigt.", "Der beschädigte Spielstand wird gelöscht.",
        "Der beschädigte Spielstand wird gelöscht und durch einen neuen ersetzt.",
        "Der Spielstand ist beschädigt. Die Sicherungskopie wird wiederhergestellt.",
        "Nicht genug Speicherplatz. Zum Speichern werden {} freier Speicherplatz benötigt.", "Es können keine weiteren Spielstände erstellt werden.",
        "Ein Fehler ist aufgetreten. ({:X})", "Unbekannter Nachrichtentyp: {}", "LADEN FEHLGESCHLAGEN",
        "Das Spiel konnte nicht gespeichert werden.", "Der Spielstand konnte nicht geladen werden.",
        "Der Spielstand konnte nicht gelöscht werden.", "Der Spielstand konnte nicht dupliziert werden.",
    },
    spanish, // ES
    { // FR
        "SAUVEGARDER", "CHARGER UNE PARTIE", "SUPPRIMER LA SAUVEGARDE", "DUPLIQUER LA SAUVEGARDE", "ÉCRASER LA SAUVEGARDE", "Créer une sauvegarde",
        "Emplacement vide", "Aucune sauvegarde", "Sauvegarde automatique", "Sauvegarde manuelle", "Sauvegarde endommagée",
        "Difficulté", "Temps de jeu", "Date de sauvegarde", "Taille", "Dossier", "État", "{}/64 SAUVEGARDES",
        "DUPLIQUER", "SUPPRIMER", "CRÉER", "SÉLECTIONNER", "RETOUR", "OUVRIR LE DOSSIER", "CONFIRMER", "Oui", "Non", "OK", "Annuler",
        "Voulez-vous sauvegarder ?", "Voulez-vous charger cette sauvegarde ?", "Voulez-vous supprimer cette sauvegarde ?",
        "Voulez-vous dupliquer cette sauvegarde ?", "Voulez-vous écraser cette sauvegarde ?",
        "Sauvegarde…", "Chargement…", "Suppression…", "Restauration de la sauvegarde…",
        "Partie sauvegardée.", "Chargement terminé.", "Suppression terminée.",
        "La sauvegarde est endommagée.", "La sauvegarde endommagée va être supprimée.",
        "La sauvegarde endommagée sera supprimée et remplacée par une nouvelle.",
        "La sauvegarde est endommagée. La copie de secours sera restaurée.",
        "Espace insuffisant pour sauvegarder. Il faut {} d’espace libre.", "Impossible de créer d’autres sauvegardes.",
        "Une erreur s’est produite. ({:X})", "Type de message inconnu : {}", "ÉCHEC DU CHARGEMENT",
        "Impossible de sauvegarder la partie.", "Impossible de charger la sauvegarde.", "Impossible de supprimer la sauvegarde.",
        "Impossible de dupliquer la sauvegarde.",
    },
    { // IT
        "SALVA PARTITA", "CARICA PARTITA", "ELIMINA SALVATAGGIO", "DUPLICA SALVATAGGIO", "SOVRASCRIVI SALVATAGGIO", "Crea nuovo salvataggio",
        "Spazio vuoto", "Nessun salvataggio", "Salvataggio automatico", "Salvataggio manuale", "Salvataggio danneggiato",
        "Difficoltà", "Tempo di gioco", "Data di salvataggio", "Dimensione", "Cartella", "Stato", "{}/64 SALVATAGGI",
        "DUPLICA", "ELIMINA", "CREA", "SELEZIONA", "INDIETRO", "APRI CARTELLA", "CONFERMA", "Sì", "No", "OK", "Annulla",
        "Vuoi salvare la partita?", "Vuoi caricare questo salvataggio?", "Vuoi eliminare questo salvataggio?",
        "Vuoi duplicare questo salvataggio?", "Vuoi sovrascrivere questo salvataggio?",
        "Salvataggio…", "Caricamento…", "Eliminazione…", "Ripristino del salvataggio…",
        "Partita salvata.", "Caricamento completato.", "Eliminazione completata.",
        "Il salvataggio è danneggiato.", "Il salvataggio danneggiato verrà eliminato.",
        "Il salvataggio danneggiato verrà eliminato e sostituito con uno nuovo.",
        "Il salvataggio è danneggiato. Verrà ripristinata la copia di sicurezza.",
        "Spazio insufficiente per salvare. Sono necessari {} liberi.", "Impossibile creare altri salvataggi.",
        "Si è verificato un errore. ({:X})", "Tipo di messaggio sconosciuto: {}", "CARICAMENTO NON RIUSCITO",
        "Impossibile salvare la partita.", "Impossibile caricare il salvataggio.", "Impossibile eliminare il salvataggio.",
        "Impossibile duplicare il salvataggio.",
    },
    { // JA
        "セーブ", "ロード", "セーブデータを削除", "セーブデータを複製", "セーブデータを上書き", "新しくセーブデータを作る",
        "空きスロット", "セーブデータがありません", "オートセーブ", "手動セーブ", "破損したセーブデータ",
        "難易度", "プレイ時間", "セーブ日時", "ファイルサイズ", "フォルダー", "状態", "セーブデータ {}/64",
        "複製", "削除", "作成", "選択", "戻る", "保存先を開く", "決定", "はい", "いいえ", "OK", "キャンセル",
        "セーブしますか？", "このセーブデータをロードしますか？", "このセーブデータを削除しますか？",
        "このセーブデータを複製しますか？", "このセーブデータを上書きしますか？",
        "セーブ中…", "ロード中…", "削除中…", "セーブデータを復元中…",
        "セーブしました。", "ロードしました。", "削除しました。",
        "セーブデータが破損しています。", "破損したセーブデータを削除します。",
        "破損したセーブデータを削除して、新しいデータを作成します。", "セーブデータが破損しています。バックアップから復元します。",
        "空き容量が不足しています。セーブには {} の空き容量が必要です。", "これ以上セーブデータを作成できません。",
        "エラーが発生しました。({:X})", "不明なメッセージの種類：{}", "ロードに失敗しました",
        "セーブできませんでした。", "セーブデータをロードできませんでした。", "セーブデータを削除できませんでした。",
        "セーブデータを複製できませんでした。",
    },
    { // KO
        "게임 저장", "게임 불러오기", "저장 데이터 삭제", "저장 데이터 복제", "저장 데이터 덮어쓰기", "새 저장 데이터 생성",
        "빈 슬롯", "저장 데이터가 없습니다", "자동 저장", "수동 저장", "손상된 저장 데이터",
        "난이도", "플레이 시간", "저장 일시", "파일 크기", "폴더", "상태", "저장 데이터 {}/64",
        "복제", "삭제", "생성", "선택", "뒤로", "저장 폴더 열기", "확인", "예", "아니요", "확인", "취소",
        "게임을 저장하시겠습니까?", "이 저장 데이터를 불러오시겠습니까?", "이 저장 데이터를 삭제하시겠습니까?",
        "이 저장 데이터를 복제하시겠습니까?", "이 저장 데이터를 덮어쓰시겠습니까?",
        "저장 중…", "불러오는 중…", "삭제 중…", "저장 데이터 복원 중…",
        "저장했습니다.", "불러오기를 완료했습니다.", "삭제했습니다.",
        "저장 데이터가 손상되었습니다.", "손상된 저장 데이터를 삭제합니다.",
        "손상된 저장 데이터를 삭제하고 새로 생성합니다.", "저장 데이터가 손상되었습니다. 백업을 복원합니다.",
        "저장 공간이 부족합니다. 여유 공간이 {} 필요합니다.", "저장 데이터를 더 만들 수 없습니다.",
        "오류가 발생했습니다. ({:X})", "알 수 없는 메시지 유형: {}", "불러오기 실패",
        "게임을 저장할 수 없습니다.", "저장 데이터를 불러올 수 없습니다.", "저장 데이터를 삭제할 수 없습니다.",
        "저장 데이터를 복제할 수 없습니다.",
    },
    spanish, // LA
    { // NL
        "SPEL OPSLAAN", "SPEL LADEN", "OPSLAGBESTAND VERWIJDEREN", "OPSLAGBESTAND KOPIËREN", "OPSLAGBESTAND OVERSCHRIJVEN", "Nieuw opslagbestand maken",
        "Lege plek", "Geen opgeslagen gegevens", "Automatisch opgeslagen", "Handmatig opgeslagen", "Beschadigd opslagbestand",
        "Moeilijkheid", "Speeltijd", "Opgeslagen op", "Bestandsgrootte", "Map", "Status", "{}/64 OPSLAGBESTANDEN",
        "KOPIËREN", "VERWIJDEREN", "MAKEN", "SELECTEREN", "TERUG", "OPSLAGMAP OPENEN", "BEVESTIGEN", "Ja", "Nee", "OK", "Annuleren",
        "Wil je het spel opslaan?", "Wil je dit opslagbestand laden?", "Wil je dit opslagbestand verwijderen?",
        "Wil je dit opslagbestand kopiëren?", "Wil je dit opslagbestand overschrijven?",
        "Bezig met opslaan…", "Bezig met laden…", "Bezig met verwijderen…", "Bezig met herstellen…",
        "Spel opgeslagen.", "Laden voltooid.", "Verwijderen voltooid.",
        "Het opslagbestand is beschadigd.", "Het beschadigde opslagbestand wordt verwijderd.",
        "Het beschadigde opslagbestand wordt verwijderd en vervangen door een nieuw bestand.",
        "Het opslagbestand is beschadigd. De reservekopie wordt hersteld.",
        "Onvoldoende ruimte om op te slaan. Er is {} vrije ruimte nodig.", "Je kunt geen opslagbestanden meer maken.",
        "Er is een fout opgetreden. ({:X})", "Onbekend berichttype: {}", "LADEN MISLUKT",
        "Het spel kon niet worden opgeslagen.", "Het opslagbestand kon niet worden geladen.",
        "Het opslagbestand kon niet worden verwijderd.", "Het opslagbestand kon niet worden gekopieerd.",
    },
    { // PL
        "ZAPISZ GRĘ", "WCZYTAJ GRĘ", "USUŃ ZAPIS", "POWIEL ZAPIS", "NADPISZ ZAPIS", "Utwórz nowy zapis",
        "Puste miejsce", "Brak zapisanych danych", "Zapis automatyczny", "Zapis ręczny", "Uszkodzony zapis",
        "Poziom trudności", "Czas gry", "Data zapisu", "Rozmiar pliku", "Folder", "Stan", "ZAPISY {}/64",
        "POWIEL", "USUŃ", "UTWÓRZ", "WYBIERZ", "WRÓĆ", "OTWÓRZ FOLDER ZAPISU", "POTWIERDŹ", "Tak", "Nie", "OK", "Anuluj",
        "Czy chcesz zapisać grę?", "Czy chcesz wczytać ten zapis?", "Czy chcesz usunąć ten zapis?",
        "Czy chcesz powielić ten zapis?", "Czy chcesz nadpisać ten zapis?",
        "Zapisywanie…", "Wczytywanie…", "Usuwanie…", "Przywracanie zapisu…",
        "Gra została zapisana.", "Wczytano zapis.", "Usunięto zapis.",
        "Zapis jest uszkodzony.", "Uszkodzony zapis zostanie usunięty.",
        "Uszkodzony zapis zostanie usunięty i zastąpiony nowym.", "Zapis jest uszkodzony. Kopia zapasowa zostanie przywrócona.",
        "Za mało miejsca na zapis. Wymagane wolne miejsce: {}.", "Nie można utworzyć więcej zapisów.",
        "Wystąpił błąd. ({:X})", "Nieznany typ komunikatu: {}", "BŁĄD WCZYTYWANIA",
        "Nie można zapisać gry.", "Nie można wczytać zapisu.", "Nie można usunąć zapisu.", "Nie można powielić zapisu.",
    },
    { // PT
        "GUARDAR JOGO", "CARREGAR JOGO", "ELIMINAR JOGO GUARDADO", "DUPLICAR JOGO GUARDADO", "SUBSTITUIR JOGO GUARDADO", "Criar novo ficheiro de dados guardados",
        "Espaço vazio", "Não há dados guardados", "Gravação automática", "Gravação manual", "Jogo guardado danificado",
        "Dificuldade", "Tempo de jogo", "Data de gravação", "Tamanho", "Pasta", "Estado", "{}/64 JOGOS GUARDADOS",
        "DUPLICAR", "ELIMINAR", "CRIAR", "SELECIONAR", "VOLTAR", "ABRIR PASTA", "CONFIRMAR", "Sim", "Não", "OK", "Cancelar",
        "Queres guardar o jogo?", "Queres carregar este jogo guardado?", "Queres eliminar este jogo guardado?",
        "Queres duplicar este jogo guardado?", "Queres substituir este jogo guardado?",
        "A guardar…", "A carregar…", "A eliminar…", "A restaurar os dados guardados…",
        "Jogo guardado.", "Carregamento concluído.", "Eliminação concluída.",
        "Os dados guardados estão danificados.", "Os dados guardados danificados serão eliminados.",
        "Os dados guardados danificados serão eliminados e substituídos por novos dados.",
        "Os dados guardados estão danificados. A cópia de segurança será restaurada.",
        "Não há espaço suficiente para guardar. É necessário ter {} de espaço livre.", "Não é possível criar mais jogos guardados.",
        "Ocorreu um erro. ({:X})", "Tipo de mensagem desconhecido: {}", "FALHA NO CARREGAMENTO",
        "Não foi possível guardar o jogo.", "Não foi possível carregar o jogo guardado.", "Não foi possível eliminar o jogo guardado.",
        "Não foi possível duplicar o jogo guardado.",
    },
    { // RU
        "СОХРАНИТЬ ИГРУ", "ЗАГРУЗИТЬ ИГРУ", "УДАЛИТЬ СОХРАНЕНИЕ", "КОПИРОВАТЬ СОХРАНЕНИЕ", "ПЕРЕЗАПИСАТЬ СОХРАНЕНИЕ", "Создать сохранение",
        "Пустая ячейка", "Нет сохранённых данных", "Автосохранение", "Ручное сохранение", "Повреждённое сохранение",
        "Уровень сложности", "Время игры", "Дата сохранения", "Размер файла", "Папка", "Состояние", "СОХРАНЕНИЯ {}/64",
        "КОПИРОВАТЬ", "УДАЛИТЬ", "СОЗДАТЬ", "ВЫБРАТЬ", "НАЗАД", "ОТКРЫТЬ ПАПКУ", "ПОДТВЕРДИТЬ", "Да", "Нет", "OK", "Отмена",
        "Сохранить игру?", "Загрузить это сохранение?", "Удалить это сохранение?", "Копировать это сохранение?", "Перезаписать это сохранение?",
        "Сохранение…", "Загрузка…", "Удаление…", "Восстановление сохранения…",
        "Игра сохранена.", "Загрузка завершена.", "Удаление завершено.",
        "Сохранение повреждено.", "Повреждённое сохранение будет удалено.",
        "Повреждённое сохранение будет удалено и заменено новым.", "Сохранение повреждено. Будет восстановлена резервная копия.",
        "Недостаточно места для сохранения. Требуется свободное место: {}.", "Достигнут предел числа сохранений.",
        "Произошла ошибка. ({:X})", "Неизвестный тип сообщения: {}", "ОШИБКА ЗАГРУЗКИ",
        "Не удалось сохранить игру.", "Не удалось загрузить сохранение.", "Не удалось удалить сохранение.", "Не удалось скопировать сохранение.",
    },
    { // TR
        "OYUNU KAYDET", "OYUNU YÜKLE", "KAYDI SİL", "KAYDI ÇOĞALT", "KAYDIN ÜZERİNE YAZ", "Yeni kayıt oluştur",
        "Boş yuva", "Kayıtlı veri yok", "Otomatik kayıt", "Manuel kayıt", "Bozuk kayıt",
        "Zorluk", "Oynanış süresi", "Kayıt tarihi", "Dosya boyutu", "Klasör", "Durum", "{}/64 KAYIT",
        "ÇOĞALT", "SİL", "OLUŞTUR", "SEÇ", "GERİ", "KAYIT KLASÖRÜNÜ AÇ", "ONAYLA", "Evet", "Hayır", "Tamam", "İptal",
        "Oyunu kaydetmek istiyor musunuz?", "Bu kaydı yüklemek istiyor musunuz?", "Bu kaydı silmek istiyor musunuz?",
        "Bu kaydı çoğaltmak istiyor musunuz?", "Bu kaydın üzerine yazmak istiyor musunuz?",
        "Kaydediliyor…", "Yükleniyor…", "Siliniyor…", "Kayıt geri yükleniyor…",
        "Oyun kaydedildi.", "Yükleme tamamlandı.", "Silme tamamlandı.",
        "Kayıtlı veri bozuk.", "Bozuk kayıt silinecek.", "Bozuk kayıt silinip yerine yeni bir kayıt oluşturulacak.",
        "Kayıtlı veri bozuk. Yedek geri yüklenecek.",
        "Kaydetmek için yeterli alan yok. {} boş alan gerekiyor.", "Daha fazla kayıt oluşturulamıyor.",
        "Bir hata oluştu. ({:X})", "Bilinmeyen mesaj türü: {}", "YÜKLEME BAŞARISIZ",
        "Oyun kaydedilemedi.", "Kayıt yüklenemedi.", "Kayıt silinemedi.", "Kayıt çoğaltılamadı.",
    },
    { // ZH
        "儲存遊戲", "載入遊戲", "刪除存檔", "複製存檔", "覆寫存檔", "建立新存檔",
        "空白欄位", "沒有存檔", "自動存檔", "手動存檔", "存檔已損毀",
        "難度", "遊玩時間", "存檔日期", "檔案大小", "資料夾", "狀態", "存檔 {}/64",
        "複製", "刪除", "建立", "選擇", "返回", "開啟存檔資料夾", "確認", "是", "否", "確定", "取消",
        "要儲存遊戲嗎？", "要載入此存檔嗎？", "要刪除此存檔嗎？", "要複製此存檔嗎？", "要覆寫此存檔嗎？",
        "正在儲存…", "正在載入…", "正在刪除…", "正在還原存檔…",
        "遊戲已儲存。", "載入完成。", "刪除完成。",
        "存檔已損毀。", "將刪除已損毀的存檔。", "將刪除已損毀的存檔，並建立新存檔。", "存檔已損毀。將還原備份。",
        "空間不足，無法儲存。需要 {} 可用空間。", "無法再建立存檔。",
        "發生錯誤。({:X})", "未知的訊息類型：{}", "載入失敗",
        "無法儲存遊戲。", "無法載入存檔。", "無法刪除存檔。", "無法複製存檔。",
    },
}};

// Message 1600, entries 1, 2 and 5, from the game's R_LANG_* WADs.
// Saves can come from any region, independently of the active language or installed WADs.
constexpr struct { std::string_view difficulty, play_time, auto_save; } metadata_labels[] = {
    {"Difficulty", "Play time", "Auto Save"}, {"Dificuldade", "Tempo de jogo", "Salvamento automático"},
    {"Schwierigkeit", "Spielzeit", "Automatische Speicherung"}, {"Dificultad", "Tiempo de juego", "Guardado automático"},
    {"Difficulté", "Temps de jeu", "Sauvegarde automatique"}, {"Difficoltà", "Tempo di gioco", "Salvataggio automatico"},
    {"難易度", "プレイ時間", "オートセーブ"}, {"난이도", "플레이 시간", "자동 저장"},
    {"Niveau", "Speeltijd", "Automatische opslagfunctie"}, {"Poziom trudności", "Czas gry", "Automatyczny zapis"},
    {"Dificuldade", "Tempo de jogo", "Guardar Automaticamente"}, {"Уровень сложности", "Время игры", "Автосохранение"},
    {"Zorluk", "Oynanış süresi", "Oto. Kayıt"}, {"難度", "遊玩時間", "自動保存"},
};

std::string_view Trim(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r";
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

} // namespace

const SaveText& GetSaveText(Gow3::UI::GameLanguage language) { return texts[static_cast<size_t>(language)]; }

SaveMetadata ParseSaveMetadata(std::string_view details) {
    SaveMetadata metadata;
    while (!details.empty()) {
        const auto end = details.find('\n');
        const auto line = details.substr(0, end);
        details.remove_prefix(end == details.npos ? details.size() : end + 1);
        const auto colon = line.find(':');
        if (colon == line.npos) continue;
        const auto label = Trim(line.substr(0, colon));
        const auto value = Trim(line.substr(colon + 1));
        for (const auto& labels : metadata_labels) {
            if (label == labels.difficulty) {
                metadata.difficulty = value;
                break;
            }
            if (label == labels.play_time) {
                metadata.play_time = value;
                break;
            }
        }
    }
    return metadata;
}

std::string_view StripAutosavePrefix(std::string_view subtitle) {
    if (!subtitle.starts_with('(')) return subtitle;
    const auto close = subtitle.find(')');
    if (close == subtitle.npos) return subtitle;
    const auto prefix = subtitle.substr(1, close - 1);
    if (std::ranges::any_of(metadata_labels, [&](const auto& labels) { return labels.auto_save == prefix; })) {
        return Trim(subtitle.substr(close + 1));
    }
    return subtitle;
}

} // namespace Libraries::SaveData::Dialog
