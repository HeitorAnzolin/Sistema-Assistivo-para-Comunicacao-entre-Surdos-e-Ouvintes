from collections import Counter, deque
import threading
import cv2
import joblib
import numpy as np
from websockets.sync.client import connect

from comum import DetectorMao, desenhar_mao, extrair_features

LIMITE_CONFIANCA = 0.8
JANELA = 25
PROPORCAO = 0.85
WS_URL = "ws://127.0.0.1:8765"

ws_conn = None
limpar_remoto = False


def escutar_ws():
    """Thread para receber comandos vindos do Arduino (ex: botao Apagar)."""
    global ws_conn, limpar_remoto
    while True:
        try:
            with connect(WS_URL) as ws:
                ws_conn = ws
                print("[WS] Conectado ao servidor_audio.py")
                for msg in ws:
                    if isinstance(msg, str) and msg == "libras_apagar":
                        limpar_remoto = True
        except Exception:
            ws_conn = None
            import time
            time.sleep(2)


def enviar_ws(mensagem: str):
    if ws_conn is not None:
        try:
            ws_conn.send(mensagem)
        except Exception as e:
            print(f"[WS] Erro ao enviar: {e}")


def atualizar_palavra_arduino(frase: str):
    enviar_ws(f"PALAVRA:{frase}")


def enviar_frase(frase: str):
    print(f"Frase pronta: {frase}")
    enviar_ws(f"falar:{frase}")


def main():
    global limpar_remoto
    threading.Thread(target=escutar_ws, daemon=True).start()

    modelo = joblib.load("modelo.joblib")
    cap = cv2.VideoCapture(0, cv2.CAP_DSHOW)

    frase = ""
    votos = deque(maxlen=JANELA)
    ultima = None
    liberado = True

    with DetectorMao() as det:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            frame = cv2.flip(frame, 1)

            # Se o usuario apertou "Apagar" no LCD do Arduino
            if limpar_remoto:
                frase, ultima, liberado = "", None, True
                limpar_remoto = False
                atualizar_palavra_arduino(frase)

            achou = det.detectar(frame)

            letra_atual, conf = None, 0.0
            if achou:
                pontos, lado = achou
                desenhar_mao(frame, pontos)

                prob = modelo.predict_proba(extrair_features(pontos, lado).reshape(1, -1))[0]
                i = int(np.argmax(prob))
                conf = float(prob[i])
                if conf >= LIMITE_CONFIANCA:
                    letra_atual = str(modelo.classes_[i])
                votos.append(letra_atual)
            else:
                votos.clear()
                liberado = True

            validos = [v for v in votos if v is not None]
            topo, n = Counter(validos).most_common(1)[0] if validos else (None, 0)
            progresso = min(1.0, n / (PROPORCAO * JANELA))

            if len(votos) == JANELA and topo is not None and progresso >= 1.0:
                if topo != ultima or liberado:
                    frase += topo
                    ultima = topo
                    liberado = False
                    votos.clear()
                    atualizar_palavra_arduino(frase)

            overlay = frame.copy()
            altura, largura = frame.shape[:2]
            cv2.rectangle(overlay, (0, 0), (largura, 80), (0, 0, 0), -1)
            cv2.rectangle(overlay, (0, altura - 85), (largura, altura), (0, 0, 0), -1)
            alpha = 0.6
            cv2.addWeighted(overlay, alpha, frame, 1 - alpha, 0, frame)

            cv2.putText(frame, f"Letra: {letra_atual or '-'} ({conf:.0%})", (15, 35),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
            cv2.rectangle(frame, (15, 50), (215, 65), (255, 255, 255), 1)
            cv2.rectangle(frame, (15, 50), (15 + int(200 * progresso), 65), (0, 255, 0), -1)
            cv2.putText(frame, f"Frase: {frase}", (15, altura - 45),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2)

            instrucoes = "ESPACO = espaco | BACKSPACE = apagar | ENTER = enviar | C = limpar | ESC = sair"
            cv2.putText(frame, instrucoes, (15, altura - 15),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 200, 200), 1)

            cv2.imshow("Libras - Construcao de Frases", frame)

            k = cv2.waitKey(1) & 0xFF
            if k == 27:  # ESC
                break
            elif k == 13 and frase:  # ENTER
                enviar_frase(frase)
                frase, ultima, liberado = "", None, True
                atualizar_palavra_arduino(frase)
            elif k == 32:  # ESPACO
                frase += " "
                ultima = None
                atualizar_palavra_arduino(frase)
            elif k == 8:  # BACKSPACE
                frase, ultima = frase[:-1], None
                atualizar_palavra_arduino(frase)
            elif k in (ord("c"), ord("C")):  # C
                frase, ultima, liberado = "", None, True
                atualizar_palavra_arduino(frase)

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()