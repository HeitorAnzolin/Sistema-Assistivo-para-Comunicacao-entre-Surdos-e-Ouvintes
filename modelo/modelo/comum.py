import os
import time
import urllib.request

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

# Letras estaticas do alfabeto de Libras.
LETRAS = list("ABCDEFGIKLMNOPQRSTUVWY")

MODELO_URL = (
    "https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
    "hand_landmarker/float16/latest/hand_landmarker.task"
)
MODELO_ARQUIVO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hand_landmarker.task")

CONEXOES = [
    (0, 1), (1, 2), (2, 3), (3, 4),
    (0, 5), (5, 6), (6, 7), (7, 8),
    (5, 9), (9, 10), (10, 11), (11, 12),
    (9, 13), (13, 14), (14, 15), (15, 16),
    (13, 17), (17, 18), (18, 19), (19, 20),
    (0, 17),
]


def garantir_modelo():
    """Baixa o modelo de deteccao de mao na primeira execucao."""
    if not os.path.exists(MODELO_ARQUIVO):
        print("Baixando hand_landmarker.task (so na primeira vez)...")
        urllib.request.urlretrieve(MODELO_URL, MODELO_ARQUIVO)


class DetectorMao:
    """Envolve o HandLandmarker do MediaPipe. Use com 'with'."""

    def __init__(self):
        garantir_modelo()
        opcoes = vision.HandLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=MODELO_ARQUIVO),
            running_mode=vision.RunningMode.VIDEO,
            num_hands=1,
            min_hand_detection_confidence=0.6,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
        self._det = vision.HandLandmarker.create_from_options(opcoes)
        self._inicio = time.monotonic()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self._det.close()

    def detectar(self, frame_bgr):
        """Retorna (pontos, lado) ou None. pontos tem shape (21, 3), coordenadas normalizadas."""
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        imagem = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        ts = int((time.monotonic() - self._inicio) * 1000)
        res = self._det.detect_for_video(imagem, ts)
        if not res.hand_landmarks:
            return None
        pontos = np.array([[p.x, p.y, p.z] for p in res.hand_landmarks[0]], dtype=np.float32)
        lado = res.handedness[0][0].category_name  # "Left" ou "Right"
        return pontos, lado


def desenhar_mao(frame, pontos):
    h, w = frame.shape[:2]
    px = [(int(x * w), int(y * h)) for x, y, _ in pontos]
    for a, b in CONEXOES:
        cv2.line(frame, px[a], px[b], (255, 255, 255), 2)
    for p in px:
        cv2.circle(frame, p, 4, (0, 120, 255), -1)


def extrair_features(pontos, lado):
    """Converte os 21 pontos da mao em um vetor de 63 valores.

    A mao e centrada no punho, escalada pelo tamanho e, se for a mao 'Left',
    espelhada no eixo x. Assim as duas maos caem na mesma orientacao e o modelo
    aprende uma vez so. Tanto a coleta quanto o reconhecimento precisam usar
    exatamente esta funcao, com o frame espelhado (cv2.flip) antes da deteccao.
    """
    pts = np.array(pontos, dtype=np.float32).copy()
    pts -= pts[0]  # punho vira a origem
    escala = np.max(np.linalg.norm(pts[:, :2], axis=1))
    if escala < 1e-6:
        escala = 1.0
    pts /= escala  # tira o efeito da distancia da camera
    if lado == "Left":
        pts[:, 0] *= -1
    return pts.flatten()
