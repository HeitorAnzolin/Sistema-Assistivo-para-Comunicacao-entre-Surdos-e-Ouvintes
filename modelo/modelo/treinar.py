"""Treina o classificador com o dataset/amostras.csv e salva modelo.joblib.

Se houver 2 ou mais pessoas no banco, a avaliacao deixa uma pessoa de fora por vez,
que e o jeito honesto de saber como o modelo vai se sair com alguem novo.
"""
from collections import Counter

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report
from sklearn.model_selection import LeaveOneGroupOut, train_test_split

rng = np.random.default_rng(0)


def criar_modelo():
    return RandomForestClassifier(n_estimators=300, n_jobs=-1, random_state=0)


def augmentar(X, y, copias=2):
    """Gira a mao um pouco (+-15 graus) e adiciona ruido leve, so no treino."""
    pts = X.reshape(-1, 21, 3)
    saidas, ys = [X], [y]
    for _ in range(copias):
        ang = np.radians(rng.uniform(-15, 15, size=(len(pts), 1)))
        c, s = np.cos(ang), np.sin(ang)
        novo = pts.copy()
        novo[:, :, 0] = pts[:, :, 0] * c - pts[:, :, 1] * s
        novo[:, :, 1] = pts[:, :, 0] * s + pts[:, :, 1] * c
        novo += rng.normal(0, 0.01, novo.shape)
        saidas.append(novo.reshape(len(pts), -1))
        ys.append(y)
    return np.vstack(saidas), np.concatenate(ys)


def avaliar(real, previsto):
    print(f"\nAcuracia: {accuracy_score(real, previsto):.3f}")
    print(classification_report(real, previsto, zero_division=0))
    erros = Counter((r, p) for r, p in zip(real, previsto) if r != p)
    if erros:
        print("Confusoes mais comuns (real -> previsto):")
        for (r, p), n in erros.most_common(8):
            print(f"  {r} -> {p}: {n}")


def main():
    df = pd.read_csv("dataset/amostras.csv")
    X = df.drop(columns=["pessoa", "letra"]).values.astype(np.float32)
    y = df["letra"].values
    grupos = df["pessoa"].values

    print("Amostras por letra:", dict(sorted(Counter(y).items())))
    poucas = [l for l, n in Counter(y).items() if n < 100]
    if poucas:
        print("Aviso: menos de 100 amostras em", sorted(poucas))

    if len(set(grupos)) >= 2:
        previsto = np.empty_like(y)
        for tr, te in LeaveOneGroupOut().split(X, y, grupos):
            Xa, ya = augmentar(X[tr], y[tr])
            previsto[te] = criar_modelo().fit(Xa, ya).predict(X[te])
        print("\nValidacao deixando uma pessoa de fora por vez:")
        avaliar(y, previsto)
    else:
        print("\nAviso: so ha uma pessoa no banco. A avaliacao abaixo e otimista,")
        print("porque treino e teste vem da mesma pessoa.")
        Xtr, Xte, ytr, yte = train_test_split(X, y, test_size=0.2, stratify=y, random_state=0)
        Xa, ya = augmentar(Xtr, ytr)
        avaliar(yte, criar_modelo().fit(Xa, ya).predict(Xte))

    Xa, ya = augmentar(X, y)
    modelo = criar_modelo().fit(Xa, ya)
    joblib.dump(modelo, "modelo.joblib")
    print("\nModelo salvo em modelo.joblib")


if __name__ == "__main__":
    main()
