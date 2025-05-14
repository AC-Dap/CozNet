import argparse

if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='DCuz')
    parser.add_argument('results', default="results.csv", help="Results CSV File")

    args = parser.parse_args()

    