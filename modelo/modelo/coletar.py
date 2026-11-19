"""Coleta de amostras para o banco proprio.

Uso:
    python coletar.py --pessoa ana
    python coletar.py --pessoa ana --fotos     (salva tambem a foto de cada amostra)

Teclas:
    letra do teclado = escolhe a letra e pausa a gravacao
    ESPACO           = comeca/para de gravar
    ESC              = sai
"""
import argparse
import csv
import os
import time

import cv2

from comum import LETRAS, DetectorMao, desenhar_mao, extrair_features


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pessoa", required=True, help="nome de quem esta fazendo os sinais")
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--intervalo", type=float, default=0.1, help="segundos entre amostras")
    ap.add_argument("--fotos", action="store_true")
    args = ap.parse_args()

    os.makedirs("dataset", exist_ok=True)
    caminho = os.path.join("dataset", "amostras.csv")
    novo = not os.path.exists(caminho)

    contagem = {l: 0 for l in LETRAS}
    if not novo:
        with open(caminho, newline="") as f:
            for linha in csv.DictReader(f):
                if linha["letra"] in contagem:
                    contagem[linha["letra"]] += 1

    cap = cv2.VideoCapture(args.camera, cv2.CAP_DSHOW)
    letra = LETRAS[0]
    gravando = False
    ultimo = 0.0

    with DetectorMao() as det, open(caminho, "a", newline="") as f:
        w = csv.writer(f)
        if novo:
            w.writerow(["pessoa", "letra"] + [f"{e}{i}" for i in range(21) for e in "xyz"])

        while True:
            ok, frame = cap.read()
            if not ok:
                break
            frame = cv2.flip(frame, 1)
            limpo = frame.copy() if args.fotos else None
            achou = det.detectar(frame)

            if achou:
                pontos, lado = achou
                desenhar_mao(frame, pontos)

                if gravando and time.time() - ultimo >= args.intervalo:
                    feats = extrair_features(pontos, lado)
                    w.writerow([args.pessoa, letra] + [f"{v:.5f}" for v in feats])
                    f.flush()
                    contagem[letra] += 1
                    ultimo = time.time()
                    if args.fotos:
                        pasta = os.path.join("dataset", "fotos", letra)
                        os.makedirs(pasta, exist_ok=True)
                        nome = f"{args.pessoa}_{int(time.time() * 1000)}.jpg"
                        cv2.imwrite(os.path.join(pasta, nome), limpo)

            cor = (0, 0, 255) if gravando else (0, 200, 0)
            estado = "GRAVANDO" if gravando else "PAUSADO"
            cv2.putText(frame, f"Letra: {letra}  ({contagem[letra]} amostras)  {estado}",
                        (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, cor, 2)
            if not achou:
                cv2.putText(frame, "mao nao detectada", (10, 60),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 165, 255), 2)
            cv2.putText(frame, "letra = escolher | ESPACO = gravar/pausar | ESC = sair",
                        (10, frame.shape[0] - 12), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
            cv2.imshow("Coleta", frame)

            k = cv2.waitKey(1) & 0xFF
            if k == 27:
                break
            elif k == 32:
                gravando = not gravando
            elif 32 < k < 127 and chr(k).upper() in LETRAS:
                letra = chr(k).upper()
                gravando = False

    cap.release()
    cv2.destroyAllWindows()
    print("Amostras por letra:", contagem)


if __name__ == "__main__":
    main()
